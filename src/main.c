#include "cinema.h"
#include "cin2.h"
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

/* Halts on a status line until the user acknowledges it. Without this,
 * the connection/format-detection log (usb connected, block size, num
 * blocks, "Cinema v2 detected", ...) is immediately overwritten by the
 * next screen (the resume prompt, the file browser, or playback itself
 * clearing to graphics mode) -- too fast to actually read. */
static void pause_for_key(void)
{
    putstr("press any key to continue");
    while (!os_GetCSC());
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
 * backed out. durations[i] is an optional "M:SS" string appended after
 * entries[i].name (empty if that file's duration couldn't be read --
 * see probe_durations), or NULL to skip showing durations entirely. */
static int run_file_browser(const fat32ro_dirent_t *entries, int count,
                             const char (*durations)[8])
{
    int selected = 0;
    int top = 0;

    for (;;) {
        int i;
        uint8_t key;

        os_ClrHome();
        putstr("Select a movie (Clear to exit)");
        for (i = top; i < count && i < top + BROWSER_VISIBLE_ROWS; ++i) {
            char line[FAT32RO_MAX_NAME + 12];

            if (durations != NULL && durations[i][0] != '\0') {
                sprintf(line, "%c%s %s", (i == selected) ? '>' : ' ',
                        entries[i].name, durations[i]);
            } else {
                sprintf(line, "%c%s", (i == selected) ? '>' : ' ', entries[i].name);
            }
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

    var = ti_Open(APPVAR_V2, "r");
    if (var) {
        uint8_t store[CIN2_RESUME_STORE_BYTES];

        /* A short read (including 0, if the appvar doesn't exist, or a
         * leftover 33-byte single-slot appvar from an older build) just
         * means "no matching resume record" -- cin2_resume_store_find
         * only ever looks inside a full CIN2_RESUME_STORE_BYTES buffer,
         * so an exact-length check here is what keeps a too-short read
         * from being scanned as if it were valid store data. */
        if (ti_Read(store, 1, sizeof(store), var) == sizeof(store)
            && cin2_resume_store_find(store, filename, &resume) >= 0
            && resume.frame_count == header->frame_count
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

/* Maps entry's cluster chain and reads/validates its CIN2 header in one
 * step -- shared by play_fat_file (which then plays it) and
 * probe_durations (which only wants frame_count/fps to compute a
 * duration string). Returns NULL and fills *out_header on success, or a
 * static, caller-printable error message on failure -- *out_map is
 * still built even on a header-validation failure (only
 * fat32ro_build_extent_map itself failing leaves it unbuilt), since a
 * caller that already checked for NULL has no reason to inspect it
 * either way. */
static const char *read_cin2_header_for_entry(global_t *global, const fat32ro_volume_t *vol,
                                                const fat32ro_dirent_t *entry,
                                                uint8_t *header_sector_buf,
                                                fat32ro_extent_map_t *out_map,
                                                cin2_header_t *out_header)
{
    uint32_t header_lba, header_run;

    if (fat32ro_build_extent_map(vol, entry->first_cluster, entry->file_size, out_map)
        != FAT32RO_SUCCESS) {
        return "error mapping file (fragmented or corrupt?)";
    }
    if (!fat32ro_extent_lookup(out_map, 0, &header_lba, &header_run)
        || msd_Read(&global->msd, header_lba, 1, header_sector_buf) != 1) {
        return "error reading movie header";
    }
    if (!cin2_has_magic(header_sector_buf) || !cin2_parse_header(header_sector_buf, out_header)) {
        return "not a valid CIN2 file";
    }

    return NULL;
}

/* "M:SS" (or "H:MM:SS" for anything an hour or longer) for the browser
 * list -- deliberately not shared with player_v2.c's own
 * format_timecode: that one always formats a *frame number* against a
 * movie's own fps, this one only ever needs a whole movie's total
 * duration, and duplicating three lines here isn't worth a shared
 * header just for that. */
static void format_duration(char *out, uint32_t frame_count, uint32_t fps_num, uint32_t fps_den)
{
    uint32_t seconds = (uint32_t)(((uint64_t)frame_count * fps_den) / fps_num);
    uint32_t hours = seconds / 3600u;

    if (hours > 0) {
        sprintf(out, "%lu:%02lu:%02lu", (unsigned long)hours,
                (unsigned long)((seconds / 60u) % 60u), (unsigned long)(seconds % 60u));
    } else {
        sprintf(out, "%lu:%02lu", (unsigned long)(seconds / 60u), (unsigned long)(seconds % 60u));
    }
}

/* Probes every playable file's header (one sector each) to fill in a
 * "M:SS" duration string per entry, shown alongside the name in the
 * browser. durations[i][0] is left '\0' for anything unreadable/invalid
 * -- the browser just shows the bare name for those, same as before this
 * existed, rather than treating it as an error this early (the file
 * might still be perfectly playable once actually selected, or might
 * not -- either way play_fat_file is what decides that). Reuses
 * header_sector/scratch_map (the same scratch buffers play_fat_file
 * uses for whichever file ends up actually selected) since nothing else
 * needs them until then. */
static void probe_durations(global_t *global, const fat32ro_volume_t *vol,
                              const fat32ro_dirent_t *playable, int playable_count,
                              uint8_t *header_sector, fat32ro_extent_map_t *scratch_map,
                              char durations[][8])
{
    int i;

    if (playable_count > 1) {
        /* A single file is about to get re-mapped anyway the instant
         * it's chosen (there's no browser wait for a 1-file list to
         * matter), so only bother with the loading message when there's
         * an actual list to sit and look at. */
        putstr("scanning movies...");
    }

    for (i = 0; i < playable_count; ++i) {
        cin2_header_t header;

        durations[i][0] = '\0';
        if (read_cin2_header_for_entry(global, vol, &playable[i], header_sector,
                                         scratch_map, &header) == NULL
            && header.width == CINEMA_V2_WIDTH && header.height == CINEMA_V2_HEIGHT) {
            format_duration(durations[i], header.frame_count, header.fps_num, header.fps_den);
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

    *played_ok = false;

    /* Mapping a large/fragmented file's cluster chain can take a
     * perceptible moment (each still-uncached FAT sector needs its own
     * device read) -- without this, the browser's selection screen just
     * sits there unchanged for that whole time, which looks exactly
     * like the app hung rather than like it's working. */
    putstr("reading file...");

    {
        const char *err = read_cin2_header_for_entry(global, vol, entry, header_sector,
                                                        movie_map, &header);
        if (err != NULL) {
            putstr(err);
            pause_for_key();
            return;
        }
    }
    if (header.width != CINEMA_V2_WIDTH || header.height != CINEMA_V2_HEIGHT) {
        putstr("unsupported CIN2 resolution");
        pause_for_key();
        return;
    }
    if (!cin2_frame_count_fits_drive(header.frame_count, movie_map->total_sectors)) {
        putstr("movie extends beyond the file's actual size");
        pause_for_key();
        return;
    }

    os_ClrHome();
    putstr("Cinema v2 (CIN2) detected");
    pause_for_key();
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
    static char durations[BROWSER_MAX_FILES][8];
    fat32ro_error_t mount_err;
    int total_count, playable_count, i;

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

    probe_durations(global, &vol, playable, playable_count, header_sector, &movie_map, durations);

    /* Loops back to the browser after a movie ends, is exited early
     * (Clear during playback), or fails to load/play -- only backing
     * out of the browser itself (Clear there) or the drive genuinely
     * going away actually leaves this function. A FAT32 drive can hold
     * several movies (that's the whole point of this browser), so
     * having to relaunch Cinema just to watch a second one would be a
     * real gap. */
    *played_ok = true;
    for (;;) {
        int choice = run_file_browser(playable, playable_count, durations);

        if (choice < 0) {
            return true; /* user backed out of the browser -- exit Cinema */
        }

        play_fat_file(global, &vol, &playable[choice], header_sector, &movie_map, played_ok);

        if (global->usb == NULL) {
            /* Drive is gone -- nothing left to browse. *played_ok
             * already reflects however play_fat_file/player_v2_run
             * left it (false, in practice: a real disconnect is always
             * reported as a failure). */
            return true;
        }
    }
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

    /* Deliberately NOT boosting to 48MHz here: usbdrvce.h documents
     * USB_TRANSFER_BUS_ERROR as most likely caused by running at a
     * non-default CPU speed, and real-hardware testing confirmed it --
     * decode got slower, not faster, at 48MHz (consistent with bus-error
     * overhead eating into the gain rather than a real speedup). Cinema
     * is USB-bound for its entire runtime, so there's no safe window to
     * run faster in. */

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

    pause_for_key();

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
            pause_for_key();
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
            /* Diagnostic dump for whenever this fallback is reached
             * unexpectedly (e.g. a drive that should have mounted as
             * FAT32 didn't) -- first 3 bytes catch a boot-sector jump
             * instruction fat32ro_mount didn't recognize, the boot
             * signature confirms whether LBA 0 even looks like a boot
             * sector/MBR at all, and the 4 partition type bytes show
             * what fat32ro_mount's MBR scan actually saw. */
            sprintf(buffer, "b0-2=%02X%02X%02X sig=%02X%02X",
                    header_sector[0], header_sector[1], header_sector[2],
                    header_sector[510], header_sector[511]);
            putstr(buffer);
            sprintf(buffer, "types=%02X,%02X,%02X,%02X",
                    header_sector[446 + 4], header_sector[462 + 4],
                    header_sector[478 + 4], header_sector[494 + 4]);
            putstr(buffer);
            pause_for_key();
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
