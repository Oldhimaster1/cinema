#ifndef CINEMA_PLAYER_V2_H
#define CINEMA_PLAYER_V2_H

#include "cinema.h"
#include "cin2.h"

/* Runs the Cinema v2 (CIN2) player: 4 persistent async read-ahead
 * slots, clock()-based rational-fps scheduling (24/1 or 24000/1001 or
 * anything else stored in the header), direct packed-4-bit-to-2x-scaled
 * decode straight into the display buffer (no sprite allocation, no
 * per-frame palette).
 *
 * *header must already be validated (cin2_parse_header succeeded), and
 * header->width/height must equal CINEMA_V2_WIDTH/HEIGHT -- this player
 * only knows how to decode that fixed geometry, so the caller must
 * check that before calling. start_frame is the frame number to begin
 * playback from (0 for the start of the movie, or a value the caller
 * loaded from the v2 resume appvar and validated against
 * header->frame_count).
 *
 * 2nd pauses/resumes, Clear exits (and saves resume state). Returns
 * true if playback ran to completion or was interrupted by the user via
 * Clear, false on a fatal error (already reported via putstr).
 */
bool player_v2_run(global_t *global, const cin2_header_t *header,
                    uint32_t start_frame);

#endif
