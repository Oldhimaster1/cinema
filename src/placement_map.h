#ifndef CINEMA_PLACEMENT_MAP_H
#define CINEMA_PLACEMENT_MAP_H
#include "cin2.h"
#include "fat32ro.h"
/* Converts a valid CPE1 descriptor into the existing FAT extent map.
 * Returns false without modifying *out on any mismatch. */
bool placement_map_from_header(const uint8_t *header_sector,
                               const fat32ro_volume_t *vol,
                               const fat32ro_dirent_t *entry,
                               fat32ro_extent_map_t *out);
#endif
