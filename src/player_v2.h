#ifndef CINEMA_PLAYER_V2_H
#define CINEMA_PLAYER_V2_H

#include "cinema.h"
#include "cin2.h"
#include "fat32ro.h"

/* The only geometry this player knows how to draw. A CIN2 frame is
 * exactly WIDTH*HEIGHT bytes, one palette index (0..15) per pixel, no
 * packing -- see src/player_v2.c's frame_slot_t comment for why v2
 * dropped 4-bit packing (it used to halve this, at the cost of a
 * per-frame unpack step that turned out to be the real bottleneck).
 * There used to be a src/decode.c/decode.h for this unpack step; now
 * that frames are raw pixels there's nothing left to decode, so these
 * constants moved here instead. DEST_WIDTH/HEIGHT is the on-screen size
 * after this player's 2x gfx_ScaledSprite_NoClip scale-up. */
#define CINEMA_V2_WIDTH        160
#define CINEMA_V2_HEIGHT        96
#define CINEMA_V2_DEST_WIDTH   (CINEMA_V2_WIDTH * 2)
#define CINEMA_V2_DEST_HEIGHT  (CINEMA_V2_HEIGHT * 2)

/* Runs the Cinema v2 (CIN2) player: persistent async read-ahead slots
 * whose buffers ARE the sprite data (frame bytes land straight from USB
 * into what gfx_ScaledSprite_NoClip draws, no copy or unpack in
 * between), clock()-based rational-fps scheduling (24/1 or 24000/1001 or
 * anything else stored in the header).
 *
 * *header must already be validated (cin2_parse_header succeeded), and
 * header->width/height must equal CINEMA_V2_WIDTH/HEIGHT -- this player
 * only knows how to decode that fixed geometry, so the caller must
 * check that before calling. start_frame is the frame number to begin
 * playback from (0 for the start of the movie, or a value the caller
 * loaded from the v2 resume appvar and validated against
 * header->frame_count).
 *
 * movie_map translates the CIN2 stream's own internal sector numbering
 * (0 = the header, 1.. = frame data, per cin2_frame_lba()) into actual
 * device LBAs. For a raw whole-device image this is the identity
 * mapping (file-relative sector N lives at device LBA N); for a file on
 * a FAT32 drive it's that file's resolved cluster-chain extents (see
 * src/fat32ro.h). Either way it must outlive this call and must cover
 * at least header->frame_count frames' worth of sectors -- the caller
 * is responsible for building/validating it (main.c does both, via
 * fat32ro_build_extent_map or a raw identity map).
 *
 * filename identifies the movie for resume purposes (see
 * cin2_resume_t): pass "" for the raw single-whole-device-image mode,
 * or the file's short name (fat32ro_dirent_t.name) when playing a file
 * off a FAT32 drive that may hold more than one movie -- otherwise two
 * different movies that happen to share a frame_count could each
 * offer to "resume" into the other's saved position.
 *
 * 2nd/Enter pauses/resumes, Left/Right/Up/Down seek, 0 restarts, Mode
 * toggles the on-screen overlay, Clear exits (and saves resume state).
 * Returns true if playback ran to completion or was interrupted by the
 * user via Clear, false on a fatal error (already reported via putstr).
 */
bool player_v2_run(global_t *global, const cin2_header_t *header,
                    uint32_t start_frame, const fat32ro_extent_map_t *movie_map,
                    const char *filename);

#endif
