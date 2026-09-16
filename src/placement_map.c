#include "placement_map.h"
#include <string.h>

bool placement_map_from_header(const uint8_t *raw,
                               const fat32ro_volume_t *vol,
                               const fat32ro_dirent_t *entry,
                               fat32ro_extent_map_t *out)
{
    cin2_placement_t p;
    cin2_placement_context_t c;
    uint16_t i;
    if (raw == NULL || vol == NULL || entry == NULL || out == NULL
        || !cin2_parse_placement(raw, &p)) return false;
    memset(&c, 0, sizeof(c));
    c.logical_sector_bytes = FAT32RO_SECTOR_BYTES;
    c.sectors_per_cluster = vol->sectors_per_cluster;
    c.volume_serial = vol->volume_serial;
    c.partition_base_lba = vol->partition_base_lba;
    c.first_fat_lba = vol->first_fat_sector;
    c.first_data_lba = vol->first_data_sector;
    c.fat_size_sectors = vol->fat_size_sectors;
    c.total_data_clusters = vol->total_clusters;
    c.movie_first_cluster = entry->first_cluster;
    c.movie_file_size = entry->file_size;
    c.immutable_header_crc = cin2_crc32(raw, CIN2_CRC_BYTES);
    if (!cin2_validate_placement(&p, &c)) return false;
    /* Validation is complete before output mutation. Populate the caller's
     * existing static movie_map directly; never allocate a second ~2 KiB
     * extent map on the calculator stack or in BSS. */
    memset(out, 0, sizeof(*out));
    out->extent_count = p.extent_count;
    out->total_sectors = p.total_movie_sectors;
    for (i = 0; i < p.extent_count; ++i) {
        out->extents[i].lba = p.extents[i].start_lba;
        out->extents[i].sectors = p.extents[i].sector_count;
    }
    return true;
}
