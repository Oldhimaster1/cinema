#include "cinema.h"
#include "cin2.h"
#include "fat32ro.h"
#include "player_v1.h"
#include "player_v2.h"
#include "cinema_settings.h"
#include "cinema_subtitle.h"
#include "placement_map.h"

#include <fileioc.h>
#include <graphx.h>
#include <msddrvce.h>
#include <tice.h>
#include <usbdrvce.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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


#define APPVAR_SETTINGS "CINSET1"
static cinema_settings_t g_settings;
static void settings_load(void){uint8_t b[CINEMA_SETTINGS_BYTES],v;cinema_settings_defaults(&g_settings);v=ti_Open(APPVAR_SETTINGS,"r");if(v){size_t n=ti_Read(b,1,sizeof(b),v);ti_Close(v);if(n==sizeof(b))cinema_settings_decode(&g_settings,b,sizeof(b));}}
static bool settings_save_verified(const cinema_settings_t*s){uint8_t b[CINEMA_SETTINGS_BYTES],r[CINEMA_SETTINGS_BYTES],v;cinema_settings_t check;cinema_settings_encode(b,s);v=ti_Open(APPVAR_SETTINGS,"w");if(!v)return false;ti_SetArchiveStatus(0,v);if(ti_Write(b,1,sizeof(b),v)!=sizeof(b)){ti_Close(v);return false;}ti_Close(v);v=ti_Open(APPVAR_SETTINGS,"r");if(!v)return false;if(ti_Read(r,1,sizeof(r),v)!=sizeof(r)){ti_Close(v);return false;}ti_Close(v);if(!cinema_settings_decode(&check,r,sizeof(r))||!cinema_settings_equal(s,&check))return false;v=ti_Open(APPVAR_SETTINGS,"r");if(v){ti_SetArchiveStatus(1,v);ti_Close(v);}return true;}
static void settings_draw(const cinema_settings_t *work, int row)
{
    char line[48];

    os_ClrHome();
    putstr("Cinema settings");
    sprintf(line, "%c Subtitles default: %s", row == 0 ? '>' : ' ',
            (work->flags & CINEMA_SETTING_SUBTITLES) ? "ON" : "OFF");
    putstr(line);
    sprintf(line, "%c Status display: %s", row == 1 ? '>' : ' ',
            (work->flags & CINEMA_SETTING_STATUS) ? "ON" : "OFF");
    putstr(line);
    sprintf(line, "%c Thumbnails: %s", row == 2 ? '>' : ' ',
            (work->flags & CINEMA_SETTING_THUMBS) ? "ON" : "OFF");
    putstr(line);
    sprintf(line, "%c Remember position: %s", row == 3 ? '>' : ' ',
            (work->flags & CINEMA_SETTING_REMEMBER) ? "ON" : "OFF");
    putstr(line);
    putstr("left/right toggles");
    putstr("enter saves, clear cancels");
}

static void settings_screen(void)
{
    cinema_settings_t work = g_settings;
    int row = 0;
    bool redraw = true;

    for (;;) {
        uint8_t key;

        if (redraw) {
            settings_draw(&work, row);
            redraw = false;
        }

        do {
            key = os_GetCSC();
        } while (key == 0);

        switch (key) {
            case sk_Up:
                row = (row + 3) % 4;
                redraw = true;
                break;
            case sk_Down:
                row = (row + 1) % 4;
                redraw = true;
                break;
            case sk_Left:
            case sk_Right:
                work.flags ^= (uint8_t)(1u << row);
                redraw = true;
                break;
            case sk_Clear:
                return;
            case sk_Enter:
                os_ClrHome();
                if (settings_save_verified(&work)) {
                    g_settings = work;
                    putstr("settings saved and verified");
                } else {
                    putstr("settings save failed");
                }
                pause_for_key();
                return;
            default:
                break;
        }
    }
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
#define V2_RESUME_CANCEL 0xFFFFFFFFu
static void make_resume_key(const cin2_header_t *header, const char *filename,
                            char out[CIN2_RESUME_FILENAME_LEN])
{
    uint8_t raw[CIN2_HEADER_BYTES];
    uint32_t crc;
    cin2_build_header(raw, header);
    crc = cin2_crc32(raw, 58);
    crc ^= cin2_crc32((const uint8_t *)filename, (uint32_t)strlen(filename));
    sprintf(out, "R%08lX", (unsigned long)crc);
}

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

    {
        char line[32];
        uint32_t seconds = (uint32_t)(((uint64_t)(resume.last_presented_frame + 1u)
            * header->fps_den) / header->fps_num);
        sprintf(line, "Continue from %lu:%02lu?", (unsigned long)(seconds / 60u),
                (unsigned long)(seconds % 60u));
        putstr(line);
    }
    putstr("2nd/Enter - Continue");
    putstr("0 - Start Over");
    putstr("Clear - Back");
    while (1) {
        uint8_t key = os_GetCSC();
        if (key == sk_0) return 0;
        if (key == sk_Clear) return V2_RESUME_CANCEL;
        if (key == sk_Enter || key == sk_2nd)
            return resume.last_presented_frame + 1u;
    }
}


#define MAP_CACHE_MAGIC 0x50414D43u
#define MAP_CACHE_VERSION 3u

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t sectors_per_cluster;
    uint16_t extent_count;
    uint32_t volume_serial;
    uint32_t partition_base_lba;
    uint32_t first_fat_sector;
    uint32_t first_data_sector;
    uint32_t fat_size_sectors;
    uint32_t total_clusters;
    uint32_t first_cluster;
    uint32_t file_size;
    uint32_t total_sectors;
    uint32_t header_crc;
    char filename[FAT32RO_MAX_NAME];
    uint8_t name_padding[3];
    uint32_t metadata_crc;
} map_cache_metadata_t;

static map_cache_metadata_t g_map_cache_metadata;
static void map_cache_appvar(char out[9], const fat32ro_volume_t *vol,
                             const fat32ro_dirent_t *entry)
{
    uint8_t key[21]; uint32_t crc;
    memset(key,0,sizeof(key)); memcpy(key,&vol->volume_serial,4);
    memcpy(key+4,&entry->first_cluster,4); memcpy(key+8,&entry->file_size,4);
    memcpy(key+12,entry->name,9); crc=cin2_crc32(key,sizeof(key));
    sprintf(out,"CM%06lX",(unsigned long)(crc&0xFFFFFFu));
}
static bool normalize_map(fat32ro_extent_map_t *map, uint32_t file_size)
{
    uint32_t need=(file_size+511u)/512u, sum=0; uint16_t i;
    if (!map->extent_count || map->extent_count>FAT32RO_MAX_EXTENTS) return false;
    for(i=0;i<map->extent_count;++i){
        uint32_t left=need-sum;
        if (!left) { map->extent_count=i; break; }
        if(map->extents[i].sectors>left) map->extents[i].sectors=left;
        sum+=map->extents[i].sectors;
    }
    map->total_sectors=sum; return sum==need;
}

static bool cached_map_is_sane(const fat32ro_volume_t *vol,
                               const fat32ro_dirent_t *entry,
                               const fat32ro_extent_map_t *map)
{
    uint16_t i;
    uint64_t sum = 0;
    uint64_t data_end = (uint64_t)vol->first_data_sector
        + (uint64_t)vol->total_clusters * vol->sectors_per_cluster;
    uint32_t needed = (entry->file_size + FAT32RO_SECTOR_BYTES - 1u)
        / FAT32RO_SECTOR_BYTES;
    if (map->extent_count == 0 || map->extent_count > FAT32RO_MAX_EXTENTS
        || map->total_sectors != needed) return false;
    for (i = 0; i < map->extent_count; ++i) {
        uint64_t start = map->extents[i].lba;
        uint64_t end = start + map->extents[i].sectors;
        if (map->extents[i].sectors == 0 || start < vol->first_data_sector
            || end > data_end) return false;
        sum += map->extents[i].sectors;
    }
    return sum == needed;
}

static bool load_cached_map(const fat32ro_volume_t *vol,
                            const fat32ro_dirent_t *entry,
                            uint32_t header_crc,
                            fat32ro_extent_map_t *out)
{
    char appvar[9]; uint8_t var; map_cache_appvar(appvar,vol,entry); var=ti_Open(appvar,"r");
    uint32_t stored_map_crc;
    uint32_t metadata_crc;
    if (!var) return false;
    if (ti_Read(&g_map_cache_metadata, 1, sizeof(g_map_cache_metadata), var)
            != sizeof(g_map_cache_metadata)) { ti_Close(var); return false; }
    metadata_crc = cin2_crc32((const uint8_t *)&g_map_cache_metadata,
        (uint32_t)offsetof(map_cache_metadata_t, metadata_crc));
    if (g_map_cache_metadata.metadata_crc != metadata_crc
        || g_map_cache_metadata.magic != MAP_CACHE_MAGIC
        || g_map_cache_metadata.version != MAP_CACHE_VERSION
        || g_map_cache_metadata.volume_serial != vol->volume_serial
        || g_map_cache_metadata.sectors_per_cluster != vol->sectors_per_cluster
        || g_map_cache_metadata.partition_base_lba != vol->partition_base_lba
        || g_map_cache_metadata.first_fat_sector != vol->first_fat_sector
        || g_map_cache_metadata.first_data_sector != vol->first_data_sector
        || g_map_cache_metadata.fat_size_sectors != vol->fat_size_sectors
        || g_map_cache_metadata.total_clusters != vol->total_clusters
        || g_map_cache_metadata.first_cluster != entry->first_cluster
        || g_map_cache_metadata.file_size != entry->file_size
        || g_map_cache_metadata.header_crc != header_crc
        || strncmp(g_map_cache_metadata.filename, entry->name, FAT32RO_MAX_NAME) != 0
        || g_map_cache_metadata.extent_count == 0
        || g_map_cache_metadata.extent_count > FAT32RO_MAX_EXTENTS) {
        ti_Close(var); return false;
    }
    memset(out, 0, sizeof(*out));
    out->extent_count = g_map_cache_metadata.extent_count;
    out->total_sectors = g_map_cache_metadata.total_sectors;
    if (ti_Read(out->extents, sizeof(out->extents[0]), out->extent_count, var)
            != out->extent_count
        || ti_Read(&stored_map_crc, sizeof(stored_map_crc), 1, var) != 1) {
        ti_Close(var); return false;
    }
    ti_Close(var);
    if (stored_map_crc != cin2_crc32((const uint8_t *)out->extents,
            (uint32_t)out->extent_count * sizeof(out->extents[0]))
        || !cached_map_is_sane(vol, entry, out)) return false;
    return true;
}

static void save_cached_map(const fat32ro_volume_t *vol,
                            const fat32ro_dirent_t *entry,
                            uint32_t header_crc,
                            const fat32ro_extent_map_t *map)
{
    char appvar[9];
    uint8_t var;
    uint32_t map_crc;
    fat32ro_extent_map_t *mutable_map=(fat32ro_extent_map_t *)map;
    if (!normalize_map(mutable_map,entry->file_size)) return;
    map_cache_appvar(appvar,vol,entry);
    memset(&g_map_cache_metadata, 0, sizeof(g_map_cache_metadata));
    g_map_cache_metadata.magic = MAP_CACHE_MAGIC;
    g_map_cache_metadata.version = MAP_CACHE_VERSION;
    g_map_cache_metadata.sectors_per_cluster = vol->sectors_per_cluster;
    g_map_cache_metadata.volume_serial = vol->volume_serial;
    g_map_cache_metadata.extent_count = map->extent_count;
    g_map_cache_metadata.partition_base_lba = vol->partition_base_lba;
    g_map_cache_metadata.first_fat_sector = vol->first_fat_sector;
    g_map_cache_metadata.first_data_sector = vol->first_data_sector;
    g_map_cache_metadata.fat_size_sectors = vol->fat_size_sectors;
    g_map_cache_metadata.total_clusters = vol->total_clusters;
    g_map_cache_metadata.first_cluster = entry->first_cluster;
    g_map_cache_metadata.file_size = entry->file_size;
    g_map_cache_metadata.total_sectors = map->total_sectors;
    g_map_cache_metadata.header_crc = header_crc;
    strncpy(g_map_cache_metadata.filename, entry->name, FAT32RO_MAX_NAME - 1);
    g_map_cache_metadata.metadata_crc = cin2_crc32((const uint8_t *)&g_map_cache_metadata,
        (uint32_t)offsetof(map_cache_metadata_t, metadata_crc));
    map_crc = cin2_crc32((const uint8_t *)map->extents,
        (uint32_t)map->extent_count * sizeof(map->extents[0]));
    var = ti_Open(appvar, "w");
    if (var) {
        bool ok = ti_Write(&g_map_cache_metadata, 1, sizeof(g_map_cache_metadata), var)
                == sizeof(g_map_cache_metadata)
            && ti_Write(map->extents, sizeof(map->extents[0]), map->extent_count, var)
                == map->extent_count
            && ti_Write(&map_crc, sizeof(map_crc), 1, var) == 1;
        if (ok) ti_SetArchiveStatus(true, var);
        ti_Close(var);
        if (!ok) ti_Delete(appvar);
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
    uint32_t header_lba;
    uint32_t header_crc;

    /* Sector zero is within the directory entry's known first cluster. Read
     * it before any chain walk so CPE1 can supply the complete validated map. */
    if (!fat32ro_first_sector_lba(vol, entry->first_cluster, &header_lba)
        || msd_Read(&global->msd, header_lba, 1, header_sector_buf) != 1) {
        return "error reading movie header";
    }
    if (!cin2_has_magic(header_sector_buf)
        || !cin2_parse_header(header_sector_buf, out_header)) {
        return "not a valid CIN2 file";
    }

    /* Fastest path: destination-prepared physical extents embedded in sector
     * zero. The adapter leaves out_map untouched unless every identity, CRC,
     * range, overlap and sector-sum check succeeds. */
    if (placement_map_from_header(header_sector_buf, vol, entry, out_map)) return NULL;

    /* Preserve the already-built calculator AppVar cache as the next level. */
    header_crc = cin2_crc32(header_sector_buf, CIN2_CRC_BYTES);
    if (load_cached_map(vol, entry, header_crc, out_map)) return NULL;

    /* Authoritative compatibility fallback: the physically verified FAT4
     * mapper with its 2 KiB data buffer in static BSS. */
    if (fat32ro_build_extent_map(vol, entry->first_cluster, entry->file_size, out_map)
        != FAT32RO_SUCCESS) {
        return "error mapping file (fragmented or corrupt?)";
    }
    save_cached_map(vol, entry, header_crc, out_map);
    return NULL;
}



#define BROWSER_UI_FG   16 /* palette index just past the movie's own 0..15 */
#define BROWSER_UI_BG   17
#define BROWSER_TEXT_X   2
#define BROWSER_TITLE_Y  4
#define BROWSER_ROW0_Y  18
#define BROWSER_ROW_H   10
#define BROWSER_THUMB_X 160
#define BROWSER_THUMB_Y 20

static uint8_t g_thumb_sprite_data[2 + CINEMA_V2_WIDTH * CINEMA_V2_HEIGHT];

/* Decodes entry's frame 0 into g_thumb_sprite_data (a real gfx_sprite_t)
 * and fills *out_palette with its movie's palette, so run_file_browser
 * can show a live preview of whichever file is currently highlighted.
 * Returns false (leaving the thumbnail blank) on anything short of a
 * clean, contiguous frame 0 -- a fragmented file's frame 0 spanning an
 * extent boundary is treated the same as "no preview available" rather
 * than pulled in with the multi-part read machinery player_v2.c uses
 * for actual playback; a missing preview isn't worth that here. */
/* Own small scratch map, deliberately NOT the caller's real movie_map:
 * a thumbnail only ever needs the header sector plus frame 0 (31
 * sectors total), so mapping just that much (see load_thumbnail below)
 * keeps this cheap regardless of how long the movie actually is --
 * walking the FULL cluster chain of, say, a 3-minute movie (tens of
 * thousands of sectors) just to read its first frame was real, needless
 * work every time the highlighted selection changed. */
static fat32ro_extent_map_t g_thumb_map;

/* V17: thumbnail map is reused for the CSU during playback. */
static fat32ro_dirent_t g_subtitle_entry;
static bool subtitle_name_for_movie(const char *movie,char out[FAT32RO_MAX_NAME])
{
    size_t len=strlen(movie),base; if(len<5||len>=FAT32RO_MAX_NAME)return false;
    strcpy(out,movie);base=len-4;out[base]='.';out[base+1]='C';out[base+2]='S';out[base+3]='U';out[base+4]=0;return true;
}
static bool subtitle_filter(const fat32ro_dirent_t *e,void *ctx)
{return !e->is_directory&&strcmp(e->name,(const char *)ctx)==0;}
static uint32_t map_subtitle_sidecar(const fat32ro_volume_t *vol, uint32_t dir_cluster,
                                    const fat32ro_dirent_t *movie)
{
    char wanted[FAT32RO_MAX_NAME];
    int count;
    bool has_more = false;
    if (!subtitle_name_for_movie(movie->name, wanted)) return 0;
    count = fat32ro_list_directory_page(vol, dir_cluster, 0, &g_subtitle_entry, 1,
                                        subtitle_filter, wanted, &has_more);
    if (count != 1 || has_more || g_subtitle_entry.file_size < CSU_HEADER_SIZE) return 0;
    if (fat32ro_build_extent_map(vol, g_subtitle_entry.first_cluster,
                                 g_subtitle_entry.file_size, &g_thumb_map) != FAT32RO_SUCCESS) return 0;
    return g_subtitle_entry.file_size;
}
static bool load_thumbnail(global_t *global, const fat32ro_volume_t *vol,
                            const fat32ro_dirent_t *entry, uint8_t *header_sector,
                            uint16_t *out_palette)
{
    cin2_header_t header;
    uint32_t lba, run;
    gfx_sprite_t *sprite = (gfx_sprite_t *)g_thumb_sprite_data;

    if (entry->is_directory) {
        return false;
    }
    if (fat32ro_build_extent_map(vol, entry->first_cluster,
            (uint32_t)CIN2_HEADER_BYTES + (uint32_t)CIN2_FRAME_SECTORS * FAT32RO_SECTOR_BYTES,
            &g_thumb_map) != FAT32RO_SUCCESS) {
        return false;
    }
    if (!fat32ro_extent_lookup(&g_thumb_map, 0, &lba, &run)
        || msd_Read(&global->msd, lba, 1, header_sector) != 1) {
        return false;
    }
    if (!cin2_has_magic(header_sector) || !cin2_parse_header(header_sector, &header)
        || header.width != CINEMA_V2_WIDTH || header.height != CINEMA_V2_HEIGHT) {
        return false;
    }
    if (!fat32ro_extent_lookup(&g_thumb_map, cin2_frame_lba_for(0, header.flags), &lba, &run)
        || run < cin2_frame_sectors(header.flags)
        || msd_Read(&global->msd, lba, cin2_frame_sectors(header.flags), sprite->data) != cin2_frame_sectors(header.flags)) {
        return false;
    }

    if (header.flags & CIN2_FLAG_PACKED4) {
        int32_t pixel;
        for (pixel = CINEMA_V2_WIDTH * CINEMA_V2_HEIGHT - 1; pixel >= 0; --pixel) {
            uint8_t packed = sprite->data[(uint32_t)pixel >> 1];
            sprite->data[pixel] = (pixel & 1) ? (packed & 0x0Fu) : (packed >> 4);
        }
    }
    sprite->width = CINEMA_V2_WIDTH;
    sprite->height = CINEMA_V2_HEIGHT;
    memcpy(out_palette, header.palette, sizeof(header.palette));
    return true;
}

/* Scrolling list in graphics mode (needed so the highlighted file's
 * thumbnail -- native 160x96, drawn via the same gfx_ScaledSprite_NoClip
 * player_v2.c uses -- can share the screen with the text list; TI-OS
 * text output and GraphX don't compose on the same screen otherwise).
 * Up/Down to move, Enter or 2nd to select, Clear to back out. Returns
 * the selected index, or -1 if the user backed out. durations[i] is an
 * optional "M:SS" string appended after entries[i].name (empty if that
 * file's duration couldn't be read -- see probe_durations), or NULL to
 * skip showing durations entirely. The browser's own UI text always
 * uses BROWSER_UI_FG/BG (set once, outside the movie palette's 0..15
 * range) so it stays legible no matter which movie's colors are
 * currently loaded into the thumbnail's palette slots. */
static int run_file_browser(global_t *global, const fat32ro_volume_t *vol,
                             const fat32ro_dirent_t *entries, int count,
                             const char (*durations)[8], const char (*formats)[4],
                             uint8_t *header_sector, int *selection_io,
                             uint32_t page_number)
{
    static const uint16_t ui_palette[2] = { 0x7FFF, 0x0000 }; /* white, black */
    int selected = (*selection_io >= 0 && *selection_io < count) ? *selection_io : 0;
    int top = selected >= BROWSER_VISIBLE_ROWS
        ? selected - BROWSER_VISIBLE_ROWS + 1 : 0;
    int thumb_index = -1;
    bool thumb_valid = false;

    gfx_Begin();
    gfx_SetPalette(ui_palette, sizeof(ui_palette), BROWSER_UI_FG);
    gfx_SetTextFGColor(BROWSER_UI_FG);
    gfx_SetTextBGColor(BROWSER_UI_BG);

    for (;;) {
        int i;
        uint8_t key;

        gfx_SetColor(BROWSER_UI_BG);
        gfx_FillRectangle_NoClip(0, 0, GFX_LCD_WIDTH, GFX_LCD_HEIGHT);
        gfx_PrintStringXY("Select movie  Clear=Back", BROWSER_TEXT_X, BROWSER_TITLE_Y);
        {
            char page_line[24];
            sprintf(page_line, "Page %lu", (unsigned long)page_number);
            gfx_PrintStringXY(page_line, BROWSER_TEXT_X, BROWSER_TITLE_Y + 9);
        }
        for (i = top; i < count && i < top + BROWSER_VISIBLE_ROWS; ++i) {
            char line[FAT32RO_MAX_NAME + 12];
            char mark = (i == selected) ? '>' : ' ';

            if (entries[i].is_directory) {
                sprintf(line, "%c[D] %s", mark, entries[i].name);
            } else if (formats != NULL && formats[i][0] != '\0'
                       && durations != NULL && durations[i][0] != '\0') {
                sprintf(line, "%c[%s] %s %s", mark, formats[i], entries[i].name,
                        durations[i]);
            } else if (formats != NULL && formats[i][0] != '\0') {
                sprintf(line, "%c[%s] %s", mark, formats[i], entries[i].name);
            } else {
                sprintf(line, "%c[?] %s", mark, entries[i].name);
            }
            gfx_PrintStringXY(line, BROWSER_TEXT_X, BROWSER_ROW0_Y + (i - top) * BROWSER_ROW_H);
        }

        if (thumb_index != selected) {
            uint16_t movie_palette[16];

            thumb_valid = load_thumbnail(global, vol, &entries[selected], header_sector,
                                          movie_palette);
            if (thumb_valid) {
                gfx_SetPalette(movie_palette, sizeof(movie_palette), 0);
            }
            thumb_index = selected;
        }
        if (thumb_valid) {
            gfx_ScaledSprite_NoClip((gfx_sprite_t *)g_thumb_sprite_data,
                                     BROWSER_THUMB_X, BROWSER_THUMB_Y, 1, 1);
        }

        do {
            key = os_GetCSC();
        } while (key == 0);

        if (key == sk_Clear) {
            *selection_io = selected;
            gfx_End();
            return -1;
        }
        if (key == sk_Mode) {
            gfx_End();
            settings_screen();
            gfx_Begin();
            gfx_SetDrawBuffer();
            gfx_SetPalette(ui_palette, sizeof(ui_palette), BROWSER_UI_FG);
            gfx_SetTextFGColor(BROWSER_UI_FG);
            gfx_SetTextBGColor(BROWSER_UI_BG);
            thumb_index = -1;
            continue;
        }
        if (key == sk_Enter || key == sk_2nd) {
            *selection_io = selected;
            gfx_End();
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

/* Header-only fast path for probe_durations: a duration only needs
 * frame_count/fps, which live in the header at file-relative sector 0 --
 * finding that sector doesn't need a whole cluster-chain walk, just the
 * O(1) cluster-to-LBA conversion fat32ro_first_sector_lba does, so
 * probing every playable file's header doesn't cost anywhere near what
 * fully extent-mapping every one of them up front would (read_cin2_
 * header_for_entry, used elsewhere, does the latter -- necessary there
 * because playback needs the whole map, wasteful here since only one of
 * these files is even going to get played). */
static const char *peek_cin2_header_fast(global_t *global, const fat32ro_volume_t *vol,
                                           const fat32ro_dirent_t *entry,
                                           uint8_t *header_sector_buf, cin2_header_t *out_header)
{
    uint32_t lba;

    if (!fat32ro_first_sector_lba(vol, entry->first_cluster, &lba)
        || msd_Read(&global->msd, lba, 1, header_sector_buf) != 1) {
        return "error reading movie header";
    }
    if (!cin2_has_magic(header_sector_buf) || !cin2_parse_header(header_sector_buf, out_header)) {
        return "not a valid CIN2 file";
    }

    return NULL;
}

/* Probes every playable file's header (one sector each, via the fast
 * path above -- no cluster-chain walk) to fill in a "M:SS" duration
 * string per entry, shown alongside the name in the browser.
 * durations[i][0] is left '\0' for anything unreadable/invalid -- the
 * browser just shows the bare name for those, same as before this
 * existed, rather than treating it as an error this early (the file
 * might still be perfectly playable once actually selected, or might
 * not -- either way play_fat_file is what decides that). Reuses
 * header_sector (the same scratch buffer play_fat_file uses for
 * whichever file ends up actually selected) since nothing else needs it
 * until then. */
static void probe_browser_metadata(global_t *global, const fat32ro_volume_t *vol,
                                    const fat32ro_dirent_t *playable, int playable_count,
                                    uint8_t *header_sector, char durations[][8],
                                    char formats[][4])
{
    int i;

    for (i = 0; i < playable_count; ++i) {
        cin2_header_t header;

        durations[i][0] = '\0';
        formats[i][0] = '\0';
        if (!playable[i].is_directory
            && peek_cin2_header_fast(global, vol, &playable[i], header_sector, &header) == NULL
            && header.width == CINEMA_V2_WIDTH && header.height == CINEMA_V2_HEIGHT) {
            format_duration(durations[i], header.frame_count, header.fps_num, header.fps_den);
            strcpy(formats[i], (header.flags & CIN2_FLAG_PACKED4) ? "P4" : "RAW");
        }
    }
}

/* Validates and plays header_sector/entry's file. Always returns true
 * (the caller already knows this is a FAT32 drive at this point, so
 * there's no "fall back to raw mode" case left) -- *played_ok reports
 * whether playback actually happened/succeeded. */
static void play_fat_file(global_t *global, const fat32ro_volume_t *vol,
                           const fat32ro_dirent_t *entry, uint8_t *header_sector,
                           fat32ro_extent_map_t *movie_map, bool *played_ok,
                           uint32_t directory_cluster)
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
        if (err != NULL) { putstr(err); pause_for_key(); return; }
    }
    if (header.width != CINEMA_V2_WIDTH || header.height != CINEMA_V2_HEIGHT) {
        putstr("unsupported CIN2 resolution");
        pause_for_key();
        return;
    }
    if (!cin2_frame_count_fits_drive_mode(header.frame_count, movie_map->total_sectors, header.flags)) {
        putstr("movie extends beyond the file's actual size");
        pause_for_key();
        return;
    }

    os_ClrHome();
    putstr("Cinema v2 (CIN2) detected");
    pause_for_key();
    {
        char resume_key[CIN2_RESUME_FILENAME_LEN];
        uint32_t start_frame;
        make_resume_key(&header, entry->name, resume_key);
        start_frame = v2_resume_menu(&header, resume_key);
        if (start_frame == V2_RESUME_CANCEL) return;
        uint32_t subtitle_size = map_subtitle_sidecar(vol,directory_cluster,entry);
        player_v2_set_packed_extra_storage(g_thumb_sprite_data,sizeof(g_thumb_sprite_data));
        player_v2_set_subtitle_stream(subtitle_size ? &g_thumb_map : NULL, subtitle_size,
            cin2_crc32(header_sector, CIN2_CRC_BYTES),
            (g_settings.flags & CINEMA_SETTING_SUBTITLES) != 0, header_sector);
        *played_ok = player_v2_run(global, &header, start_frame, movie_map, resume_key);
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
/* first_cluster sentinel for the synthetic ".." entry prepended to a
 * subfolder's listing: 0 is never a valid data cluster (real clusters
 * start at 2), so it can't collide with a real directory. */
#define BROWSER_UP_CLUSTER 0
#define BROWSER_PREV_CLUSTER 1
#define BROWSER_NEXT_CLUSTER 0xFFFFFFFFu
#define BROWSER_PAGE_REAL 29
#define BROWSER_MAX_DEPTH  8

static bool browser_entry_filter(const fat32ro_dirent_t *e, void *ctx)
{
    (void)ctx; return e->is_directory || has_playable_extension(e->name);
}
static bool try_fat32_multi_file(global_t *global, uint8_t *header_sector, bool *played_ok)
{
    fat32ro_volume_t vol;
    static fat32ro_dirent_t playable[BROWSER_MAX_FILES];
    static fat32ro_extent_map_t movie_map;
    static char durations[BROWSER_MAX_FILES][8];
    static char formats[BROWSER_MAX_FILES][4];
    static uint32_t dir_stack[BROWSER_MAX_DEPTH];
    fat32ro_error_t mount_err;
    uint32_t current_dir;
    int dir_depth = 0;
    uint32_t page_skip = 0;
    int browser_selection = 0;
    static uint32_t page_stack[BROWSER_MAX_DEPTH];

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

    current_dir = vol.root_cluster;
    *played_ok = true;

    /* Loops back to the browser -- in whichever folder is currently
     * open -- after a movie ends, is exited early (Clear during
     * playback), fails to load/play, or the user navigates up/down a
     * folder. Only backing out of the browser at the root (Clear there)
     * or the drive genuinely going away actually leaves this function. */
    for (;;) {
        int real_count, playable_count, choice; bool has_more=false;
        playable_count=0;
        if (dir_depth>0) { strcpy(playable[playable_count].name,".."); playable[playable_count].first_cluster=BROWSER_UP_CLUSTER; playable[playable_count].file_size=0; playable[playable_count++].is_directory=true; }
        if (page_skip>0) { strcpy(playable[playable_count].name,"<PREV>"); playable[playable_count].first_cluster=BROWSER_PREV_CLUSTER; playable[playable_count].file_size=0; playable[playable_count++].is_directory=true; }
        real_count=fat32ro_list_directory_page(&vol,current_dir,page_skip,
            playable+playable_count,BROWSER_PAGE_REAL,browser_entry_filter,NULL,&has_more);
        if(real_count<0){putstr("error reading FAT32 directory");*played_ok=false;return true;}
        playable_count+=real_count;
        if(has_more){strcpy(playable[playable_count].name,"<NEXT>");playable[playable_count].first_cluster=BROWSER_NEXT_CLUSTER;playable[playable_count].file_size=0;playable[playable_count++].is_directory=true;}
        if (playable_count == 0) {
            os_ClrHome();
            putstr("FAT32 drive detected");
            putstr("No .bin/.cin movies found in the root folder");
            *played_ok = false;
            return true;
        }

        probe_browser_metadata(global, &vol, playable, playable_count,
                               header_sector, durations, formats);
        choice = run_file_browser(global, &vol, playable, playable_count, durations,
                                  formats, header_sector, &browser_selection,
                                  page_skip / BROWSER_PAGE_REAL + 1u);

        if (choice < 0) {
            if (dir_depth > 0) {
                current_dir = dir_stack[--dir_depth]; page_skip=page_stack[dir_depth];
                continue;
            }
            return true; /* backed out of the browser at the root -- exit Cinema */
        }

        if (playable[choice].first_cluster == BROWSER_PREV_CLUSTER) { page_skip = page_skip >= BROWSER_PAGE_REAL ? page_skip-BROWSER_PAGE_REAL : 0; browser_selection=0; continue; }
        if (playable[choice].first_cluster == BROWSER_NEXT_CLUSTER) { page_skip += BROWSER_PAGE_REAL; browser_selection=0; continue; }
        if (playable[choice].first_cluster == BROWSER_UP_CLUSTER && playable[choice].is_directory) {
            current_dir = dir_stack[--dir_depth]; page_skip=page_stack[dir_depth]; browser_selection=0; continue;
        }
        if (playable[choice].is_directory) {
            if (dir_depth < BROWSER_MAX_DEPTH) {
                dir_stack[dir_depth] = current_dir; page_stack[dir_depth++] = page_skip;
                current_dir = playable[choice].first_cluster; page_skip=0; browser_selection=0;
            }
            /* A folder nested deeper than BROWSER_MAX_DEPTH is simply
             * not descended into -- reported nowhere specially, since
             * pressing Enter on it just reopens the same listing, which
             * is self-explanatory enough not to need its own message. */
            continue;
        }

        play_fat_file(global, &vol, &playable[choice], header_sector, &movie_map, played_ok, current_dir);

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

    settings_load();

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
            if (!cin2_frame_count_fits_drive_mode(v2_header.frame_count, msdinfo.bnum, v2_header.flags))
            {
                putstr("movie extends beyond drive capacity");
                goto msd_error;
            }

            os_ClrHome();
            putstr("Cinema v2 (CIN2) detected");
            pause_for_key();
            {
                char resume_key[CIN2_RESUME_FILENAME_LEN];
                uint32_t start_frame;
                make_resume_key(&v2_header, "", resume_key);
                start_frame = v2_resume_menu(&v2_header, resume_key);
                if (start_frame == V2_RESUME_CANCEL) goto normal_cleanup;
                build_raw_identity_map(&raw_map, msdinfo.bnum);
                player_ok = player_v2_run(&global, &v2_header, start_frame, &raw_map, resume_key);
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

normal_cleanup:
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

