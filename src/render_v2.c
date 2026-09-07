#include "render_v2.h"
#include "player_v2.h"

#include <graphx.h>

/* Cinema always scales exactly one fixed thing: a CINEMA_V2_WIDTH x
 * CINEMA_V2_HEIGHT (160x96), one-byte-per-pixel (palette index) frame,
 * 2x nearest-neighbor to 320x192, at destination (0, 24), into whichever
 * buffer GraphX is currently targeting. gfx_ScaledSprite_NoClip has to
 * handle an arbitrary source size, scale factor, and destination
 * coordinate on every call; this doesn't need to, so every one of those
 * becomes a compile-time constant here instead of a value it has to
 * check or compute per frame (or per pixel) -- no clipping math, no
 * scale-factor math, no destination-coordinate math, no generic sprite
 * dispatch.
 *
 * This deliberately reads each compact 160-byte source row TWICE (once
 * per destination row it produces) rather than writing the first
 * expanded 320-byte destination row and then copying *that* back out of
 * the framebuffer to produce the second identical row:
 *
 *   source row  -> expand -> dest row 1
 *   source row  -> expand -> dest row 2   (source re-read, not dest row 1)
 *
 * not:
 *
 *   source row  -> expand -> dest row 1
 *   dest row 1  -> copy   -> dest row 2   (framebuffer read-back)
 *
 * Framebuffer reads can carry real wait-state cost on this hardware, so
 * re-reading 160 bytes from ordinary RAM is cheaper than reading back
 * 320 bytes that were *just* written to video memory. 96 source-row
 * passes this way cost 2*160 = 320 source reads and 2*320 = 640
 * destination writes per source row (30,720 source reads / 61,440
 * destination writes total).
 *
 * Two things this first-draft version got wrong, found by actually
 * compiling it with the real CE toolchain at -O3 and reading the
 * generated assembly (see the renderer phase's design notes) rather
 * than assuming the compiler would do the right thing:
 *
 *   1. Writing each row's destination offset as "y * 2 * stride" (and
 *      the source offset as "y * width") computed the same way every
 *      loop iteration, from scratch. Even at -O3 the compiler did not
 *      turn that into a running total -- it emitted a genuine `call
 *      __imulu` (software multiply) once per source row, 96 times per
 *      frame. Rewritten below to carry d0/d1/row as pointers that just
 *      advance by a fixed stride each iteration, which needs no
 *      multiply at all, per the "avoid at runtime: multiplication"
 *      goal for this renderer.
 *   2. The 160-pixel inner loop was a plain `for` loop with one
 *      conditional branch per pixel (a decrement-and-compare on every
 *      single byte). Unrolled into blocks of 16 below (160 / 16 = 10
 *      exact blocks, no remainder) so the branch/loop-overhead cost is
 *      paid once per 16 pixels instead of once per pixel. 16 was picked
 *      as a middle ground the phase's design notes explicitly asked for
 *      (32/16/8 all considered) -- real hardware timing would be needed
 *      to say which unroll factor is actually fastest, which is not
 *      something this development environment can measure (see the
 *      phase report for why).
 *
 * Writes go through the public gfx_vbuffer macro (graphx.h), which
 * resolves through GraphX's own current-buffer pointer rather than a
 * fixed physical address, so this always draws into whichever buffer
 * gfx_SetDraw()/gfx_SetDrawBuffer()/gfx_SetDrawScreen() last selected --
 * exactly like every other GraphX drawing call. */

#define RENDER_SRC_W        CINEMA_V2_WIDTH   /* 160 */
#define RENDER_SRC_H        CINEMA_V2_HEIGHT  /* 96  */
#define RENDER_DST_STRIDE   GFX_LCD_WIDTH     /* 320 */
#define RENDER_DST_Y_OFFSET ((GFX_LCD_HEIGHT - CINEMA_V2_DEST_HEIGHT) / 2) /* 24 */

#define RENDER_UNROLL 16 /* RENDER_SRC_W / RENDER_UNROLL must be exact (160/16 = 10) */

/* Duplicates s[i] into out[2*i] and out[2*i+1]. A macro (not a helper
 * function) so the 16 calls below are genuinely 16 separate, inlined
 * load/store pairs in the generated code, not a call in a loop. */
#define RENDER_DUP2(i) \
    do { \
        unsigned char p_ = s[i]; \
        out[(i) * 2] = p_; \
        out[(i) * 2 + 1] = p_; \
    } while (0)

/* Expands exactly RENDER_UNROLL (16) source bytes starting at s into
 * 2*RENDER_UNROLL (32) destination bytes starting at out. Caller
 * advances s by RENDER_UNROLL and out by 2*RENDER_UNROLL between calls. */
static void expand_block16(const unsigned char *s, unsigned char *out)
{
    RENDER_DUP2(0);  RENDER_DUP2(1);  RENDER_DUP2(2);  RENDER_DUP2(3);
    RENDER_DUP2(4);  RENDER_DUP2(5);  RENDER_DUP2(6);  RENDER_DUP2(7);
    RENDER_DUP2(8);  RENDER_DUP2(9);  RENDER_DUP2(10); RENDER_DUP2(11);
    RENDER_DUP2(12); RENDER_DUP2(13); RENDER_DUP2(14); RENDER_DUP2(15);
}

void render_scaled_fixed_c(const unsigned char *src)
{
    const unsigned char *row = src;
    unsigned char *d0 = (unsigned char *)gfx_vbuffer
        + (unsigned)RENDER_DST_Y_OFFSET * RENDER_DST_STRIDE;
    unsigned char *d1 = d0 + RENDER_DST_STRIDE;
    unsigned y;

    for (y = 0; y < RENDER_SRC_H; ++y) {
        unsigned block;
        const unsigned char *s = row;
        unsigned char *out = d0;

        for (block = 0; block < RENDER_SRC_W / RENDER_UNROLL; ++block) {
            expand_block16(s, out);
            s += RENDER_UNROLL;
            out += RENDER_UNROLL * 2;
        }

        s = row; /* re-read the compact source row -- see header comment
                     above -- instead of reading d0 back. */
        out = d1;
        for (block = 0; block < RENDER_SRC_W / RENDER_UNROLL; ++block) {
            expand_block16(s, out);
            s += RENDER_UNROLL;
            out += RENDER_UNROLL * 2;
        }

        row += RENDER_SRC_W;
        d0 += 2u * RENDER_DST_STRIDE;
        d1 += 2u * RENDER_DST_STRIDE;
    }
}
