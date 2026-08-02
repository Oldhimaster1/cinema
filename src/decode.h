#ifndef CINEMA_DECODE_H
#define CINEMA_DECODE_H

/* Pure pixel math: packed 4-bit source -> nearest-neighbor 2x scaled
 * destination. No calculator-specific headers on purpose, so this file
 * (and decode.c) can be compiled and unit-tested on a host machine with
 * a plain C compiler -- see tests/test_decode.c. */

#include <stdint.h>

#define CINEMA_V2_WIDTH        160
#define CINEMA_V2_HEIGHT        96
#define CINEMA_V2_PACKED_BYTES ((CINEMA_V2_WIDTH * CINEMA_V2_HEIGHT) / 2)
#define CINEMA_V2_DEST_WIDTH   (CINEMA_V2_WIDTH * 2)
#define CINEMA_V2_DEST_HEIGHT  (CINEMA_V2_HEIGHT * 2)

/* Decodes CINEMA_V2_PACKED_BYTES bytes of packed 4-bit pixel data
 * (row-major, high nibble = even column, low nibble = odd column -- see
 * docs/CIN2_FORMAT.md) directly into a 2x-nearest-neighbor-scaled block
 * of `framebuffer`, starting at row `y_offset`.
 *
 * `framebuffer` must have at least `y_offset + CINEMA_V2_DEST_HEIGHT` rows
 * and each row must be at least CINEMA_V2_DEST_WIDTH bytes wide (on
 * calculator, `framebuffer` is `gfx_vbuffer`, whose rows are exactly
 * CINEMA_V2_DEST_WIDTH == 320 bytes wide). Row width is fixed at compile
 * time (matches gfx_vbuffer's row stride) via the framebuffer_stride
 * parameter so this function has no GraphX dependency.
 */
void cinema_draw_packed4_scaled2x(const uint8_t *packed,
                                   uint8_t *framebuffer,
                                   uint16_t framebuffer_stride,
                                   uint16_t y_offset);

#endif
