#include "fat32ro.h"
#include <stddef.h>

#define FAT32_EOC_MIN      0x0FFFFFF8u
#define FAT32_BAD_CLUSTER  0x0FFFFFF7u
#define FAT32_ENTRY_MASK   0x0FFFFFFFu

static uint16_t read_u16le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32le(const uint8_t *p)
{
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static bool cluster_is_valid_data_cluster(const fat32ro_volume_t *vol, uint32_t cluster)
{
    return cluster >= 2 && cluster < vol->total_clusters + 2;
}

static uint32_t cluster_to_lba(const fat32ro_volume_t *vol, uint32_t cluster)
{
    return vol->first_data_sector + (cluster - 2) * (uint32_t)vol->sectors_per_cluster;
}

static fat32ro_error_t read_fat_entry(const fat32ro_volume_t *vol, uint32_t cluster,
                                       uint32_t *out_next)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = vol->first_fat_sector + fat_offset / FAT32RO_SECTOR_BYTES;
    uint32_t entry_offset = fat_offset % FAT32RO_SECTOR_BYTES;
    uint8_t buf[FAT32RO_SECTOR_BYTES];

    if (vol->read_sectors(vol->ctx, fat_sector, 1, buf) != 1) {
        return FAT32RO_ERROR_READ_FAILED;
    }

    *out_next = read_u32le(buf + entry_offset) & FAT32_ENTRY_MASK;
    return FAT32RO_SUCCESS;
}

/* --- mount ------------------------------------------------------------ */

typedef struct {
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint32_t fat_size_32;
    uint32_t root_cluster;
    uint32_t total_sectors_32;
} bpb_fields_t;

static bool is_valid_sectors_per_cluster(uint8_t v)
{
    return v == 1 || v == 2 || v == 4 || v == 8 || v == 16
        || v == 32 || v == 64 || v == 128;
}

/* Parses and sanity-checks the fields this module needs from a FAT32
 * BPB (BIOS Parameter Block). Returns false if any field is out of the
 * range a real FAT32 volume could have -- used both to validate a
 * boot sector we already suspect is one, and to positively distinguish
 * "this is a FAT32 boot sector" from "this is actually an MBR" when we
 * don't yet know which LBA 0 holds (see fat32ro_mount). */
static bool parse_bpb(const uint8_t *sector, bpb_fields_t *out)
{
    out->bytes_per_sector = read_u16le(sector + 11);
    out->sectors_per_cluster = sector[13];
    out->reserved_sectors = read_u16le(sector + 14);
    out->num_fats = sector[16];
    out->fat_size_32 = read_u32le(sector + 36);
    out->root_cluster = read_u32le(sector + 44);
    out->total_sectors_32 = read_u32le(sector + 32);

    if (out->bytes_per_sector != FAT32RO_SECTOR_BYTES) {
        return false;
    }
    if (!is_valid_sectors_per_cluster(out->sectors_per_cluster)) {
        return false;
    }
    if (out->reserved_sectors == 0) {
        return false;
    }
    if (out->num_fats == 0 || out->num_fats > 8) {
        return false;
    }
    /* FATSz32 == 0 means this is actually a FAT12/16 BPB (which uses
     * FATSz16 instead) -- not something we support. */
    if (out->fat_size_32 == 0) {
        return false;
    }
    if (out->root_cluster < 2) {
        return false;
    }
    if (out->total_sectors_32 == 0) {
        return false;
    }

    return true;
}

fat32ro_error_t fat32ro_mount(fat32ro_volume_t *vol,
                               fat32ro_read_sectors_t read_sectors, void *ctx)
{
    uint8_t sector[FAT32RO_SECTOR_BYTES];
    uint32_t base_lba = 0;
    bpb_fields_t bpb;

    if (vol == NULL || read_sectors == NULL) {
        return FAT32RO_ERROR_INVALID_PARAM;
    }

    if (read_sectors(ctx, 0, 1, sector) != 1) {
        return FAT32RO_ERROR_READ_FAILED;
    }
    if (read_u16le(sector + 510) != 0x55AA) {
        return FAT32RO_ERROR_NOT_FAT32;
    }

    if (!parse_bpb(sector, &bpb)) {
        /* LBA 0 isn't a FAT32 boot sector itself -- try it as an MBR
         * and look for a FAT32 partition (type 0x0B "FAT32 CHS" or
         * 0x0C "FAT32 LBA") among its 4 entries. */
        bool found = false;
        int i;

        for (i = 0; i < 4 && !found; ++i) {
            const uint8_t *entry = sector + 446 + i * 16;
            uint8_t type = entry[4];

            if (type == 0x0B || type == 0x0C) {
                base_lba = read_u32le(entry + 8);
                found = true;
            }
        }
        if (!found) {
            return FAT32RO_ERROR_NOT_FAT32;
        }

        if (read_sectors(ctx, base_lba, 1, sector) != 1) {
            return FAT32RO_ERROR_READ_FAILED;
        }
        if (read_u16le(sector + 510) != 0x55AA || !parse_bpb(sector, &bpb)) {
            return FAT32RO_ERROR_BAD_BPB;
        }
    }

    vol->read_sectors = read_sectors;
    vol->ctx = ctx;
    vol->partition_base_lba = base_lba;
    vol->first_fat_sector = base_lba + bpb.reserved_sectors;
    vol->fat_size_sectors = bpb.fat_size_32;
    vol->first_data_sector = vol->first_fat_sector
        + (uint32_t)bpb.num_fats * bpb.fat_size_32;
    vol->sectors_per_cluster = bpb.sectors_per_cluster;
    vol->root_cluster = bpb.root_cluster;

    {
        uint32_t system_sectors = bpb.reserved_sectors
            + (uint32_t)bpb.num_fats * bpb.fat_size_32;

        if (bpb.total_sectors_32 <= system_sectors) {
            return FAT32RO_ERROR_BAD_BPB;
        }
        vol->total_clusters =
            (bpb.total_sectors_32 - system_sectors) / bpb.sectors_per_cluster;
    }

    if (!cluster_is_valid_data_cluster(vol, vol->root_cluster)) {
        return FAT32RO_ERROR_BAD_BPB;
    }

    return FAT32RO_SUCCESS;
}

/* --- directory listing -------------------------------------------------- */

static void format_short_name(const uint8_t *raw_entry, char *out)
{
    uint8_t i;
    uint8_t len = 0;

    for (i = 0; i < 8 && raw_entry[i] != ' '; ++i) {
        out[len++] = (char)raw_entry[i];
    }
    if (raw_entry[8] != ' ') {
        out[len++] = '.';
        for (i = 8; i < 11 && raw_entry[i] != ' '; ++i) {
            out[len++] = (char)raw_entry[i];
        }
    }
    out[len] = '\0';
}

int fat32ro_list_root(const fat32ro_volume_t *vol, fat32ro_dirent_t *out, int max_entries)
{
    uint32_t cluster;
    uint32_t steps = 0;
    int count = 0;

    if (vol == NULL || out == NULL || max_entries <= 0) {
        return -(int)FAT32RO_ERROR_INVALID_PARAM;
    }

    cluster = vol->root_cluster;
    if (!cluster_is_valid_data_cluster(vol, cluster)) {
        return -(int)FAT32RO_ERROR_CLUSTER_CHAIN;
    }

    for (;;) {
        uint8_t sector_in_cluster;

        for (sector_in_cluster = 0; sector_in_cluster < vol->sectors_per_cluster;
             ++sector_in_cluster) {
            uint8_t buf[FAT32RO_SECTOR_BYTES];
            uint32_t lba = cluster_to_lba(vol, cluster) + sector_in_cluster;
            int entry_in_sector;

            if (vol->read_sectors(vol->ctx, lba, 1, buf) != 1) {
                return -(int)FAT32RO_ERROR_READ_FAILED;
            }

            for (entry_in_sector = 0; entry_in_sector < FAT32RO_SECTOR_BYTES / 32;
                 ++entry_in_sector) {
                const uint8_t *raw = buf + entry_in_sector * 32;
                uint8_t first = raw[0];
                uint8_t attr = raw[11];

                if (first == 0x00) {
                    return count; /* end of directory */
                }
                if (first == 0xE5) {
                    continue; /* deleted entry */
                }
                if ((attr & 0x0F) == 0x0F) {
                    continue; /* long-filename fragment; unsupported by design */
                }
                if (attr & 0x18) { /* volume label (0x08) or directory (0x10) */
                    continue;
                }

                if (count < max_entries) {
                    format_short_name(raw, out[count].name);
                    out[count].first_cluster =
                        ((uint32_t)read_u16le(raw + 20) << 16) | read_u16le(raw + 26);
                    out[count].file_size = read_u32le(raw + 28);
                    count++;
                }
                /* Past max_entries: keep scanning so end-of-directory /
                 * corruption is still detected correctly, just stop
                 * recording -- the returned count is capped, not the scan. */
            }
        }

        {
            uint32_t next;
            fat32ro_error_t err;

            if (++steps > vol->total_clusters) {
                return -(int)FAT32RO_ERROR_CLUSTER_CHAIN;
            }
            err = read_fat_entry(vol, cluster, &next);
            if (err != FAT32RO_SUCCESS) {
                return -(int)err;
            }
            if (next >= FAT32_EOC_MIN) {
                return count; /* chain ended without an explicit 0x00 marker --
                                  legitimate if the directory exactly fills its
                                  last cluster */
            }
            if (!cluster_is_valid_data_cluster(vol, next)) {
                return -(int)FAT32RO_ERROR_CLUSTER_CHAIN;
            }
            cluster = next;
        }
    }
}

/* --- extent map ---------------------------------------------------------- */

/* True if `cluster` falls within any extent already recorded in `out`.
 * A well-formed FAT cluster chain never revisits a cluster; a chain
 * that does is corrupt (cyclic), and without this check such a chain
 * can otherwise pass silently -- each revisit still contributes
 * `sectors_per_cluster` toward the running total, so a short cycle can
 * "satisfy" an arbitrary requested size by re-reading the same physical
 * sectors repeatedly, producing an extent map that looks valid but
 * points at the wrong data for most of the file. */
static bool cluster_already_used(const fat32ro_extent_map_t *out, const fat32ro_volume_t *vol,
                                  uint32_t cluster)
{
    uint16_t i;

    for (i = 0; i < out->extent_count; ++i) {
        uint32_t start_cluster = 2
            + (out->extents[i].lba - vol->first_data_sector) / vol->sectors_per_cluster;
        uint32_t length_clusters = out->extents[i].sectors / vol->sectors_per_cluster;

        if (cluster >= start_cluster && cluster < start_cluster + length_clusters) {
            return true;
        }
    }

    return false;
}

static fat32ro_error_t close_run(fat32ro_extent_map_t *out, const fat32ro_volume_t *vol,
                                  uint32_t run_start_cluster, uint32_t run_length_clusters)
{
    if (out->extent_count >= FAT32RO_MAX_EXTENTS) {
        return FAT32RO_ERROR_TOO_FRAGMENTED;
    }

    out->extents[out->extent_count].lba = cluster_to_lba(vol, run_start_cluster);
    out->extents[out->extent_count].sectors =
        run_length_clusters * (uint32_t)vol->sectors_per_cluster;
    out->total_sectors += out->extents[out->extent_count].sectors;
    out->extent_count++;

    return FAT32RO_SUCCESS;
}

fat32ro_error_t fat32ro_build_extent_map(const fat32ro_volume_t *vol,
                                          uint32_t first_cluster,
                                          uint32_t file_size_bytes,
                                          fat32ro_extent_map_t *out)
{
    uint32_t needed_sectors;
    uint32_t cluster;
    uint32_t run_start_cluster;
    uint32_t run_length_clusters;
    uint32_t steps = 0;

    if (vol == NULL || out == NULL) {
        return FAT32RO_ERROR_INVALID_PARAM;
    }

    out->extent_count = 0;
    out->total_sectors = 0;

    if (file_size_bytes == 0) {
        return FAT32RO_SUCCESS; /* legitimately empty; no clusters needed */
    }
    if (!cluster_is_valid_data_cluster(vol, first_cluster)) {
        return FAT32RO_ERROR_CLUSTER_CHAIN;
    }

    needed_sectors = (file_size_bytes + FAT32RO_SECTOR_BYTES - 1) / FAT32RO_SECTOR_BYTES;

    cluster = first_cluster;
    run_start_cluster = first_cluster;
    run_length_clusters = 1;

    while ((uint64_t)out->total_sectors
           + (uint64_t)run_length_clusters * vol->sectors_per_cluster
           < needed_sectors) {
        uint32_t next;
        fat32ro_error_t err;

        if (++steps > vol->total_clusters) {
            return FAT32RO_ERROR_CLUSTER_CHAIN;
        }

        err = read_fat_entry(vol, cluster, &next);
        if (err != FAT32RO_SUCCESS) {
            return err;
        }
        if (!cluster_is_valid_data_cluster(vol, next)) {
            /* Covers both a corrupt/bad cluster reference and the chain
             * ending (EOC) before providing enough sectors for the
             * file's declared size -- either way the directory entry
             * and the chain disagree, so refuse rather than read
             * garbage past the chain. */
            return FAT32RO_ERROR_CLUSTER_CHAIN;
        }
        if ((next >= run_start_cluster && next < run_start_cluster + run_length_clusters)
            || cluster_already_used(out, vol, next)) {
            /* `next` revisits a cluster already claimed by this same
             * walk -- a cyclic/corrupt chain. See cluster_already_used. */
            return FAT32RO_ERROR_CLUSTER_CHAIN;
        }

        if (next == cluster + 1) {
            run_length_clusters++;
        } else {
            fat32ro_error_t close_err =
                close_run(out, vol, run_start_cluster, run_length_clusters);
            if (close_err != FAT32RO_SUCCESS) {
                return close_err;
            }
            run_start_cluster = next;
            run_length_clusters = 1;
        }

        cluster = next;
    }

    return close_run(out, vol, run_start_cluster, run_length_clusters);
}

bool fat32ro_extent_lookup(const fat32ro_extent_map_t *map, uint32_t sector_offset,
                            uint32_t *out_lba, uint32_t *out_run_sectors)
{
    uint32_t base = 0;
    uint16_t i;

    if (sector_offset >= map->total_sectors) {
        return false;
    }

    for (i = 0; i < map->extent_count; ++i) {
        uint32_t extent_sectors = map->extents[i].sectors;

        if (sector_offset < base + extent_sectors) {
            uint32_t offset_within_extent = sector_offset - base;

            *out_lba = map->extents[i].lba + offset_within_extent;
            *out_run_sectors = extent_sectors - offset_within_extent;
            return true;
        }
        base += extent_sectors;
    }

    return false; /* unreachable if total_sectors is consistent with extents[] */
}
