#include "cinema.h"
#include "cin2.h"
#include "decode.h"
#include "fat32ro.h"
#include "player_v1.h"
#include "player_v2.h"

#include <fileioc.h>
#include <msddrvce.h>
#include <tice.h>
#include <usbdrvce.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BROWSER_MAX_FILES     32
#define BROWSER_VISIBLE_ROWS   7

void putstr(const char *str)
{
    os_PutStrFull((char *)str);
    os_NewLine();
}

usb_error_t handleUsbEvent(usb_event_t event, void *event_data,
                            usb_callback_data_t *global)
{
    switch (event)
    {
        case USB_DEVICE_DISCONNECTED_EVENT:
            putstr("usb device disconnected");
            if (global->usb)
                msd_Close(&global->msd);
            global->usb = NULL;
            break;
        case USB_DEVICE_CONNECTED_EVENT:
            putstr("usb device connected");
            return usb_ResetDevice(event_data);
        case USB_DEVICE_ENABLED_EVENT:
            global->usb = event_data;
            putstr("usb device enabled");
            break;
        case USB_DEVICE_DISABLED_EVENT:
            putstr("usb device disabled");
            return USB_RETRY_INIT;
        default:
            break;
    }

    return USB_SUCCESS;
}

/* Adapts msd_Read to fat32ro's read_sectors callback shape. */
static uint32_t fat_read_adapter(void *ctx, uint32_t lba, uint32_t count, void *buffer)
{
    global_t *global = (global_t *)ctx;

    return msd_Read(&global->msd, lba, count, buffer);
}

/* An extent map that's just "file-relative sector N lives at device LBA
 * N" -- i.e. the original raw-whole-device-image layout, expressed in
 * the same movie_map shape the player now always takes, so player_v2.c
 * doesn't need two different code paths for "raw image" vs "file on a
 * FAT32 drive". */
static void build_raw_identity_map(fat32ro_extent_map_t *map, uint32_t drive_sectors)
{
    map->extent_count = 1;
    map->extents[0].lba = 0;
    map->extents[0].sectors = drive_sectors;
    map->total_sectors = drive_sectors;
}

static bool ext_matches(const char *ext, const char *want3)
{
    uint8_t i;

    for (i = 0; i < 3; ++i) {
        char c = ext[i];

        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c != want3[i]) {
            return false;
        }
    }

    return true;
}

/* Playable files are recognized by extension only (.bin or .cin,
 * case-insensitive) -- the file browser is deliberately simple (root
 * directory only, no metadata preview), matching what a normal FAT32
 * drive might otherwise be used for too (a user's other files on the
 * same drive are just not shown, not disturbed). */
static bool has_playable_extension(const char *name)
{
    size_t len = strlen(name);

    if (len < 4 || name[len - 4] != '.') {
        return false;
    }

    return ext_matches(name + len - 3, "BIN") || ext_matches(name + len - 3, "CIN");
}

/* Simple scrolling text list: Up/Down to move, Enter or 2nd to select,
 * Clear to back out. Returns the selected index, or -1 if the user
 * backed out. */
static int run_file_browser(const fat32ro_dirent_t *entries, int count)
{
    int selected = 0;
    int top = 0;

    for (;;) {
        int i;
        uint8_t key;

        os_ClrHome();
        putstr("Select a movie (Clear to exit)");
        for (i = top; i < count && i < top + BROWSER_VISIBLE_ROWS; ++i) {
            char line[FAT32RO_MAX_NAME + 1];

            sprintf(line, "%c%s", (i == selected) ? '>' : ' ', entries[i].name);
            putstr(line);
        }

        do {
            key = os_GetCSC();
        } while (key == 0);

        if (key == sk_Clear) {
            return -1;
        }
        if (key == sk_Enter || key == sk_2nd) {
            return selected;
        }
        if (key == sk_Up && selected > 0) {
            selected--;
        } else if (key == sk_Down && selected < count - 1) {
            selected++;
        }

        if (selected < top) {
            top = selected;
        }
        if (selected >= top + BROWSER_VISIBLE_ROWS) {
            top = selected - BROWSER_VISIBLE_ROWS + 1;
        }
    }
}

/* Prompts "resume where you left off?" and, if the user says yes and a
 * valid v1 resume record exists, returns the saved palette LBA.
 * Otherwise returns 0 (start of movie). Mirrors the original Cinema's
 * inline resume menu exactly -- same appvar, same 4-byte raw-LBA
 * format, same prompt -- so existing v1 resume state keeps working. */
static uint32_t v1_resume_menu(void)
{
    uint32_t start_lba = 0;
    uint8_t var = ti_Open(APPVAR_V1, "r+");

    if (var) {
        putstr("Resume where you left off?");
        putstr("Other - YES        0 - NO");

        while (1) {
            uint8_t key = os_GetCSC();
            if (key) {
                if (key != sk_0) {
                    ti_Read(&start_lba, sizeof(uint32_t), 1, var);
                    ti_SetGCBehavior(NULL, NULL);
                    ti_SetArchiveStatus(1, var);
                }
                break;
            }
        }
        ti_Close(var);
    }

    return start_lba;
}

/* Same idea as v1_resume_menu but for the v2 (CIN2) resume record. Only
 * offers to resume if a record exists, parses successfully, was saved
 * against a movie with the same frame count AND the same filename
 * (filename is "" for the raw single-image mode, so that mode's
 * behavior is unchanged) -- otherwise a stale or mismatched record is
 * silently discarded and playback starts from frame 0, rather than
 * risking seeking into a differently-encoded (or simply different)
 * movie. */
static uint32_t v2_resume_menu(const cin2_header_t *header, const char *filename)
{
    uint8_t var;
    cin2_resume_t resume;
    bool have_resume = false;

    var = ti_Open(APPVAR_V2, "r+");
    if (var) {
        uint8_t raw[CIN2_RESUME_BYTES];

        if (ti_Read(raw, 1, CIN2_RESUME_BYTES, var) == CIN2_RESUME_BYTES
            && cin2_parse_resume_record(raw, &resume)
            && resume.frame_count == header->frame_count
            && strcmp(resume.filename, filename) == 0
            && resume.last_presented_frame + 1 < header->frame_count) {
            have_resume = true;
        }
        ti_Close(var);
    }

    if (!have_resume) {
        return 0;
    }

    putstr("Resume where you left off?");
    putstr("Other - YES        0 - NO");

    while (1) {
        uint8_t key = os_GetCSC();
        if (key) {
            if (key == sk_0) {
                return 0;
            }
            return resume.last_presented_frame + 1;
        }
    }
}

/* Validates and plays header_sector/entry's file. Always returns true
 * (the caller already knows this is a FAT32 drive at this point, so
 * there's no "fall back to raw mode" case left) -- *played_ok reports
 * whether playback actually happened/succeeded. */
static void play_fat_file(global_t *global, const fat32ro_volume_t *vol,
                           const fat32ro_dirent_t *entry, uint8_t *header_sector,
                           fat32ro_extent_map_t *movie_map, bool *played_ok)
{
    cin2_header_t header;
    uint32_t header_lba, header_run;

    *played_ok = false;

    if (fat32ro_build_extent_map(vol, entry->first_cluster, entry->file_size, movie_map)
        != FAT32RO_SUCCESS) {
        putstr("error mapping file (fragmented or corrupt?)");
        return;
    }
    if (!fat32ro_extent_lookup(movie_map, 0, &header_lba, &header_run)
        || msd_Read(&global->msd, header_lba, 1, header_sector) != 1) {
        putstr("error reading movie header");
        return;
    }
    if (!cin2_has_magic(header_sector) || !cin2_parse_header(header_sector, &header)) {
        putstr("not a valid CIN2 file");
        return;
    }
    if (header.width != CINEMA_V2_WIDTH || header.height != CINEMA_V2_HEIGHT) {
        putstr("unsupported CIN2 resolution");
        return;
    }
    if (!cin2_frame_count_fits_drive(header.frame_count, movie_map->total_sectors)) {
        putstr("movie extends beyond the file's actual size");
        return;
    }

    os_ClrHome();
    putstr("Cinema v2 (CIN2) detected");
    {
        uint32_t start_frame = v2_resume_menu(&header, entry->name);

        *played_ok = player_v2_run(global, &header, start_frame, movie_map, entry->name);
    }
}

/* Attempts to mount a FAT32 filesystem and, if it holds at least one
 * playable file, lets the user browse and pick one. Returns true if
 * ANY recognizable filesystem/partition was found (FAT32 or otherwise),
 * regardless of whether a movie was actually played -- the caller
 * should NOT then also attempt raw single-image detection in that case,
 * since real filesystem metadata would be misinterpreted as movie data
 * (an unsupported filesystem is reported to the user directly instead).
 * Returns false only when there's no recognizable filesystem at all
 * (fat32ro_mount's FAT32RO_ERROR_NOT_FAT32), in which case *played_ok is
 * unset and the caller should fall back to raw detection -- that's the
 * one situation still ambiguous enough to plausibly be a raw v1/v2
 * whole-device image instead of a drive Cinema simply can't read. */
static bool try_fat32_multi_file(global_t *global, uint8_t *header_sector, bool *played_ok)
{
    fat32ro_volume_t vol;
    static fat32ro_dirent_t entries[BROWSER_MAX_FILES];
    static fat32ro_dirent_t playable[BROWSER_MAX_FILES];
    static fat32ro_extent_map_t movie_map;
    fat32ro_error_t mount_err;
    int total_count, playable_count, i, choice;

    mount_err = fat32ro_mount(&vol, fat_read_adapter, global);
    if (mount_err == FAT32RO_ERROR_NOT_FAT32) {
        /* Genuinely no recognizable filesystem/partition at all -- the
         * one case that could plausibly be a raw whole-device v1/v2
         * image instead, so let the caller try that. */
        return false;
    }
    if (mount_err != FAT32RO_SUCCESS) {
        /* A real filesystem/partition IS present -- just not one this
         * module supports (FAT16, exFAT, NTFS, ...), or the boot sector
         * couldn't be read at all. Report that plainly rather than
         * silently falling through to raw v1/v2 detection, which would
         * misinterpret filesystem metadata as movie data and fail in a
         * far more confusing way ("Cinema v1 (legacy) drive detected"
         * followed by an instant, unexplained exit). */
        os_ClrHome();
        if (mount_err == FAT32RO_ERROR_UNSUPPORTED_FILESYSTEM) {
            putstr("drive has a filesystem Cinema can't read");
            putstr("(FAT16/exFAT/NTFS?) - reformat it as FAT32");
        } else {
            putstr("error reading drive filesystem");
        }
        *played_ok = false;
        return true;
    }

    total_count = fat32ro_list_root(&vol, entries, BROWSER_MAX_FILES);
    if (total_count < 0) {
        putstr("error reading FAT32 root directory");
        *played_ok = false;
        return true;
    }

    playable_count = 0;
    for (i = 0; i < total_count && playable_count < BROWSER_MAX_FILES; ++i) {
        if (has_playable_extension(entries[i].name)) {
            playable[playable_count++] = entries[i];
        }
    }

    if (playable_count == 0) {
        os_ClrHome();
        putstr("FAT32 drive detected");
        putstr("No .bin/.cin movies found in the root folder");
        *played_ok = false;
        return true;
    }

    choice = run_file_browser(playable, playable_count);
    if (choice < 0) {
        *played_ok = true; /* user chose to back out; not an error */
        return true;
    }

    play_fat_file(global, &vol, &playable[choice], header_sector, &movie_map, played_ok);
    return true;
}

int main(void)
{
    static char buffer[212];
    static global_t global;
    static uint8_t header_sector[BLOCK_SIZE];
    static fat32ro_extent_map_t raw_map;
    usb_error_t usberr;
    msd_error_t msderr;
    msd_info_t msdinfo;
    cin2_header_t v2_header;
    bool is_v2 = false;
    bool player_ok = true;

    memset(&global, 0, sizeof(global_t));
    os_SetCursorPos(1, 0);

    /* usb initialization loop; waits for something to be plugged in */
    do
    {
        global.usb = NULL;

        usberr = usb_Init(handleUsbEvent, &global, NULL, USB_DEFAULT_INIT_FLAGS);
        if (usberr != USB_SUCCESS)
        {
            putstr("usb init error.");
            goto usb_error;
        }

        while (usberr == USB_SUCCESS)
        {
            if (global.usb != NULL)
                break;

            if (os_GetCSC())
            {
                putstr("exiting cinema, press a key");
                goto usb_error;
            }

            usberr = usb_WaitForInterrupt();
        }
    } while (usberr == USB_RETRY_INIT);

    if (usberr != USB_SUCCESS)
    {
        putstr("usb enable error.");
        goto usb_error;
    }

    msderr = msd_Open(&global.msd, global.usb);
    if (msderr != MSD_SUCCESS)
    {
        putstr("failed opening msd");
        goto usb_error;
    }

    putstr("opened msd");

    msderr = msd_Info(&global.msd, &msdinfo);
    if (msderr != MSD_SUCCESS)
    {
        putstr("error getting msd info");
        goto msd_error;
    }

    sprintf(buffer, "block size: %u bytes", (uint24_t)msdinfo.bsize);
    putstr(buffer);
    sprintf(buffer, "num blocks: %u", (uint24_t)msdinfo.bnum);
    putstr(buffer);

    if (msdinfo.bsize != BLOCK_SIZE)
    {
        putstr("unsupported block size");
        goto msd_error;
    }

    if (!try_fat32_multi_file(&global, header_sector, &player_ok))
    {
        /* Not a FAT32 drive -- fall back to the original raw
         * single-whole-device-image detection, unchanged, for existing
         * raw-imaged drives. */
        if (msd_Read(&global.msd, 0, 1, header_sector) != 1)
        {
            putstr("error reading drive header");
            goto msd_error;
        }

        is_v2 = cin2_has_magic(header_sector);
        if (is_v2)
        {
            if (!cin2_parse_header(header_sector, &v2_header))
            {
                putstr("corrupt or unsupported CIN2 header");
                goto msd_error;
            }
            if (v2_header.width != CINEMA_V2_WIDTH
                || v2_header.height != CINEMA_V2_HEIGHT)
            {
                putstr("unsupported CIN2 resolution");
                goto msd_error;
            }
            if (!cin2_frame_count_fits_drive(v2_header.frame_count, msdinfo.bnum))
            {
                putstr("movie extends beyond drive capacity");
                goto msd_error;
            }

            os_ClrHome();
            putstr("Cinema v2 (CIN2) detected");
            {
                uint32_t start_frame = v2_resume_menu(&v2_header, "");

                build_raw_identity_map(&raw_map, msdinfo.bnum);
                player_ok = player_v2_run(&global, &v2_header, start_frame, &raw_map, "");
            }
        }
        else
        {
            os_ClrHome();
            putstr("Cinema v1 (legacy) drive detected");
            {
                uint32_t start_lba = v1_resume_menu();
                player_ok = player_v1_run(&global, start_lba);
            }
        }
    }

    msd_Close(&global.msd);
    usb_Cleanup();

    if (!player_ok) {
        while (!os_GetCSC());
    }

    return 0;

msd_error:
    msd_Close(&global.msd);
    usb_Cleanup();

    while (!os_GetCSC());

    return 0;

usb_error:
    usb_Cleanup();

    while (!os_GetCSC());

    return 0;
}
