#ifndef CINEMA_FAT32RO_H
#define CINEMA_FAT32RO_H

/* Minimal read-only FAT32 support: just enough to list files in a
 * drive's root directory and resolve a chosen file's cluster chain into
 * a list of raw-sector (LBA, count) extents.
 *
 * This intentionally does NOT use the CE toolchain's official fatdrvce
 * library: fatdrvce's file-read API (fat_ReadFile) is synchronous only
 * -- there is no async completion callback -- so routing per-frame
 * playback reads through it would give up the overlapped
 * read-ahead/render pipeline the v2 player already has (see
 * src/player_v2.c). Extent resolution lets playback keep using
 * msd_ReadAsync exactly as it already does for a raw single-movie
 * image, just against sector numbers looked up from an extent table
 * instead of a fixed formula.
 *
 * This is a from-scratch implementation of the FAT32 on-disk format
 * itself (Microsoft's published, stable specification), not a
 * reverse-engineering of any other implementation's internal
 * structures -- the two are different in kind: one is a public
 * standard, the other would be unsupported guessing.
 *
 * Deliberately out of scope (kept simple on purpose):
 *   - Subdirectories. Only the root directory is listed.
 *   - Long filenames (VFAT LFN entries are recognized and skipped, not
 *     assembled) -- files must have a plain 8.3-compatible short name,
 *     which is entirely within our control since we control what name
 *     the encoder writes.
 *   - Writing anything. This module never issues a write.
 *
 * No calculator-specific headers are included here on purpose, so this
 * file is host-testable like src/cin2.c and src/decode.c -- see
 * tests/test_fat32ro.c. The caller supplies a sector-read callback
 * (mirroring msd_Read's shape exactly) so the same code runs against a
 * synthetic in-memory disk image on a host machine and against the real
 * msd_t on the calculator.
 */

#include <stdbool.h>
#include <stdint.h>

#define FAT32RO_SECTOR_BYTES 512

/* Bounded, not a hard spec limit: a file split across more contiguous
 * runs than this is rejected with a clear error rather than silently
 * truncated or allowed to exhaust memory. 256 extents comfortably
 * covers even a fairly fragmented file; a fresh/lightly used drive with
 * one video copied at a time will typically produce exactly 1. */
#define FAT32RO_MAX_EXTENTS 256

/* Root directory listing is bounded the same way -- a fixed-size
 * caller-provided array, not a dynamic list. */
#define FAT32RO_MAX_NAME 13 /* "12345678.123" + NUL */

typedef uint32_t (*fat32ro_read_sectors_t)(void *ctx, uint32_t lba,
                                            uint32_t count, void *buffer);

typedef struct {
    fat32ro_read_sectors_t read_sectors;
    void *ctx;

    uint32_t partition_base_lba;
    uint32_t first_fat_sector;   /* absolute LBA */
    uint32_t first_data_sector;  /* absolute LBA */
    uint32_t fat_size_sectors;
    uint32_t root_cluster;
    uint32_t total_clusters;     /* data-region cluster count; bounds chain walks */
    uint8_t sectors_per_cluster;
} fat32ro_volume_t;

typedef enum {
    FAT32RO_SUCCESS = 0,
    FAT32RO_ERROR_READ_FAILED,      /* read_sectors returned short */
    FAT32RO_ERROR_NOT_FAT32,        /* no recognizable filesystem/partition at all -- most
                                        likely a genuinely raw/unformatted drive (or one holding
                                        a raw whole-device image), the one case the caller
                                        should still try interpreting as such */
    FAT32RO_ERROR_UNSUPPORTED_FILESYSTEM, /* a real, recognizable filesystem/partition IS
                                        present (a FAT12/16 boot sector, or an MBR partition
                                        typed NTFS/exFAT/FAT16/etc.) but it isn't FAT32 -- unlike
                                        FAT32RO_ERROR_NOT_FAT32, the caller should NOT then guess
                                        this might be a raw v1/v2 image; it's a real filesystem
                                        this module just doesn't support */
    FAT32RO_ERROR_BAD_BPB,          /* boot sector parsed but BPB fields are nonsensical */
    FAT32RO_ERROR_UNSUPPORTED_SECTOR_SIZE, /* BPB_BytsPerSec != 512 */
    FAT32RO_ERROR_CLUSTER_CHAIN,    /* bad/reserved cluster, or a chain that doesn't terminate
                                        within total_clusters+1 steps (loop protection) */
    FAT32RO_ERROR_TOO_FRAGMENTED,   /* more contiguous runs than FAT32RO_MAX_EXTENTS */
    FAT32RO_ERROR_INVALID_PARAM,
} fat32ro_error_t;

/* Reads and validates the boot sector (handling both a superfloppy
 * layout, where the boot sector is at LBA 0, and an MBR-partitioned
 * layout, where LBA 0 is a partition table pointing at the real boot
 * sector), and fills in *vol. */
fat32ro_error_t fat32ro_mount(fat32ro_volume_t *vol,
                               fat32ro_read_sectors_t read_sectors, void *ctx);

typedef struct {
    char name[FAT32RO_MAX_NAME]; /* reconstructed "NAME.EXT", trailing spaces trimmed */
    uint32_t first_cluster;
    uint32_t file_size;
} fat32ro_dirent_t;

/* Lists up to max_entries plain files (not directories, volume labels,
 * or LFN fragments) from the root directory into out[], returning the
 * count found (which may be less than what actually exists if the
 * directory has more than max_entries qualifying files -- callers that
 * care should size their array generously; this is a simple flat
 * browser, not a paged one). Returns a negative fat32ro_error_t on
 * failure (e.g. a corrupt directory chain), 0 or more on success. */
int fat32ro_list_root(const fat32ro_volume_t *vol, fat32ro_dirent_t *out, int max_entries);

typedef struct {
    uint32_t lba;
    uint32_t sectors;
} fat32ro_extent_t;

typedef struct {
    fat32ro_extent_t extents[FAT32RO_MAX_EXTENTS];
    uint16_t extent_count;
    uint32_t total_sectors; /* sum of extents[i].sectors; = ceil(file_size / 512) */
} fat32ro_extent_map_t;

/* Walks first_cluster's chain and fills *out with the raw-sector
 * extents backing the first file_size_bytes of that chain (a file's
 * last cluster is normally larger than its declared size; only the
 * sectors needed to cover file_size_bytes are included). */
fat32ro_error_t fat32ro_build_extent_map(const fat32ro_volume_t *vol,
                                          uint32_t first_cluster,
                                          uint32_t file_size_bytes,
                                          fat32ro_extent_map_t *out);

/* Translates a 0-based sector offset within a file (e.g. "sector 16 of
 * this file's data") into an absolute device LBA and how many
 * consecutive sectors remain available from there in the same extent
 * (i.e. before a fragmentation boundary or end of file). Returns false
 * if sector_offset is beyond the mapped extent (out->total_sectors). */
bool fat32ro_extent_lookup(const fat32ro_extent_map_t *map, uint32_t sector_offset,
                            uint32_t *out_lba, uint32_t *out_run_sectors);

#endif
