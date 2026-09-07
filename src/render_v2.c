#include "render_v2.h"
#include "player_v2.h"

#include <graphx.h>

volatile unsigned long g_render_calls[4];

void record_render_call(int renderer_id)
{
    g_render_calls[renderer_id]++;
}

/* See render_v2.h's routing-proof comment for why this wrapper exists:
 * gfx_ScaledSprite_NoClip is OS library code (see the renderer phase's
 * report -- graphx.lib is a jump-table manifest, not code this project
 * can instrument), so this is the closest equivalent to "instrumented
 * inside the renderer function" available for the GraphX path. */
void render_scaled_graphx(const struct gfx_sprite_t *sprite)
{
    record_render_call(CINEMA_RENDERER_ID_GRAPHX);
    gfx_ScaledSprite_NoClip((const gfx_sprite_t *)sprite, 0,
                             (GFX_LCD_HEIGHT - CINEMA_V2_DEST_HEIGHT) / 2, 2, 2);
}

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
/* Reads one source byte and writes it into both destination rows (two
 * bytes each, four writes total) before advancing all three pointers. A
 * macro, not a helper, so the 16 calls below are genuinely inlined,
 * independent steps.
 *
 * This replaced an earlier version that (per the renderer phase's
 * design notes) read each compact source row *twice* -- once while
 * writing destination row 1, once again while writing destination row
 * 2 -- specifically to avoid reading the just-written destination row
 * back out of (possibly slower) video memory. That's still avoided
 * here: this never reads any destination byte, ever, from either row.
 * It also does something the two-pass version didn't: write both
 * destination rows from the *same* source read instead of visiting
 * every source byte twice. Disassembling the two-pass version showed
 * why that mattered here -- 66 push + 66 pop instructions in its
 * 16-pixel block (confirmed by a real compile with the toolchain),
 * because keeping two pointers (source, one destination row) live for
 * indexed/incremented access needs an offset-addressable register each,
 * and eZ80 only has one spare (iy; ix is the frame pointer), forcing
 * constant swapping. Three pointers here (source, row-1 dest, row-2
 * dest) still fit without any of that: none of them need offset
 * addressing, just plain sequential register-pair access (hl/de/bc),
 * which eZ80 has three of. Confirmed by disassembling this version:
 * zero push/pop in the block body (see render_v2.c's phase report for
 * the exact before/after instruction counts) -- changing the algorithm
 * shape, not just the C spelling of the same one, is what actually
 * removed the register-shuffling cost the first version paid. */
#define RENDER_DUP2ROWS() \
    do { \
        unsigned char p_ = *s++; \
        *out0++ = p_; \
        *out0++ = p_; \
        *out1++ = p_; \
        *out1++ = p_; \
    } while (0)

/* Expands exactly RENDER_UNROLL (16) source bytes starting at s into
 * 2*RENDER_UNROLL (32) bytes in *each* of two destination rows starting
 * at out0/out1. s, out0, out1 are local copies (passed by value); the
 * caller advances its own copies by RENDER_UNROLL / 2*RENDER_UNROLL
 * between calls independently. */
static void expand_block16_both_rows(const unsigned char *s,
                                       unsigned char *out0, unsigned char *out1)
{
    RENDER_DUP2ROWS(); RENDER_DUP2ROWS(); RENDER_DUP2ROWS(); RENDER_DUP2ROWS();
    RENDER_DUP2ROWS(); RENDER_DUP2ROWS(); RENDER_DUP2ROWS(); RENDER_DUP2ROWS();
    RENDER_DUP2ROWS(); RENDER_DUP2ROWS(); RENDER_DUP2ROWS(); RENDER_DUP2ROWS();
    RENDER_DUP2ROWS(); RENDER_DUP2ROWS(); RENDER_DUP2ROWS(); RENDER_DUP2ROWS();
}

void render_scaled_fixed_c(const unsigned char *src)
{
    const unsigned char *row = src;
    record_render_call(CINEMA_RENDERER_ID_FIXED_C);
    unsigned char *d0 = (unsigned char *)gfx_vbuffer
        + (unsigned)RENDER_DST_Y_OFFSET * RENDER_DST_STRIDE;
    unsigned y;

    for (y = 0; y < RENDER_SRC_H; ++y) {
        unsigned block;
        const unsigned char *s = row;
        unsigned char *out0 = d0;
        unsigned char *out1 = d0 + RENDER_DST_STRIDE;

        for (block = 0; block < RENDER_SRC_W / RENDER_UNROLL; ++block) {
            expand_block16_both_rows(s, out0, out1);
            s += RENDER_UNROLL;
            out0 += RENDER_UNROLL * 2;
            out1 += RENDER_UNROLL * 2;
        }

        row += RENDER_SRC_W;
        d0 += 2u * RENDER_DST_STRIDE;
    }
}
