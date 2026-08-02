#ifndef CINEMA_PLAYER_V1_H
#define CINEMA_PLAYER_V1_H

#include "cinema.h"
#include <stdint.h>

/* Legacy Cinema format: per-frame 256-color palette (1 sector) + raw
 * 160x96 8bpp image (30 sectors) = 31 sectors/frame. No header -- LBA 0
 * is frame 0's palette directly. See docs/CIN2_FORMAT.md for why v2
 * exists; this is kept byte-for-byte compatible with existing v1 drives
 * and existing "SSCINEMA" resume appvars.
 *
 * start_lba is the palette LBA to begin playback from (0 for the start
 * of the movie, or a value loaded from the v1 resume appvar).
 *
 * Returns true if playback ran to normal completion (or was interrupted
 * by the user), false if a fatal error occurred (already reported via
 * putstr by the time this returns).
 */
bool player_v1_run(global_t *global, uint32_t start_lba);

#endif
