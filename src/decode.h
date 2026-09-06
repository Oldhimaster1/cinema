#ifndef CINEMA_DECODE_H
#define CINEMA_DECODE_H

/* Pure pixel math: unpacks packed 4-bit source pixels into plain 8bpp
 * (one byte per pixel) output. No calculator-specific headers on
 * purpose, so this file (and decode.c) can be compiled and unit-tested
 * on a host machine with a plain C compiler -- see tests/test_decode.c.
 *
 * This deliberately does NOT do any scaling: an earlier version of this
 * function unpacked directly into a 2x-nearest-neighbor-scaled region of
 * the LCD framebuffer, in a hand-written loop. Real-hardware testing
 * showed that loop was much slower than Cinema's own v1 (legacy) player
 * achieves for the same 2x scale factor -- v1 hands that job to
 * GraphX's own gfx_ScaledSprite_NoClip() instead of scaling pixels by
 * hand. Splitting the work the same way (unpack once at native
 * resolution here, let player_v2.c's caller scale the result via
 * GraphX) is both simpler and lets the calculator-optimized library
 * routine do what it's already good at.
 */

#include <stdint.h>

#define CINEMA_V2_WIDTH        160
#define CINEMA_V2_HEIGHT        96
#define CINEMA_V2_PACKED_BYTES ((CINEMA_V2_WIDTH * CINEMA_V2_HEIGHT) / 2)

/* On-screen size after the caller's own 2x scaling -- not used by
 * decode.c itself (which only ever produces a native CINEMA_V2_WIDTH x
 * CINEMA_V2_HEIGHT buffer), but shared here so callers positioning that
 * scaled output (e.g. player_v2.c's OSD layout) have one definition to
 * agree on. */
#define CINEMA_V2_DEST_WIDTH   (CINEMA_V2_WIDTH * 2)
#define CINEMA_V2_DEST_HEIGHT  (CINEMA_V2_HEIGHT * 2)

/* Unpacks CINEMA_V2_PACKED_BYTES bytes of packed 4-bit pixel data
 * (row-major, high nibble = even column, low nibble = odd column -- see
 * docs/CIN2_FORMAT.md) into CINEMA_V2_WIDTH * CINEMA_V2_HEIGHT plain
 * palette-index bytes (one per pixel, native resolution, no scaling),
 * written starting at `out`. `out` must have room for that many bytes,
 * contiguous (on calculator, this is a gfx_sprite_t's data[] -- see
 * player_v2.c). */
void cinema_unpack_packed4(const uint8_t *packed, uint8_t *out);

#endif
