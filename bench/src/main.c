/* Render-only benchmark: times gfx_ScaledSprite_NoClip() against
 * whichever fixed renderer this was built with (CINEMA_RENDERER=fixed_c
 * or fixed_asm -- see ../makefile), on the same fixed 160x96 test frame,
 * with no USB/FAT/scheduler work anywhere near the timed region. See
 * ../makefile's header comment for why this is its own tiny program
 * rather than a mode inside Cinema.
 *
 * Bounded: exactly BENCH_ITERATIONS passes of each renderer, no more.
 * Escapable: waits for an explicit keypress before exiting, at every
 * stage -- it never enters an unbounded loop of its own accord. */
#include "player_v2.h"
#include "render_v2.h"

#include <graphx.h>
#include <tice.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BENCH_ITERATIONS 100

/* gfx_sprite_t-shaped, matching frame_slot_t's layout in player_v2.c
 * (2-byte width/height header, then raw pixel data) -- gfx_ScaledSprite_
 * NoClip needs that shape; render_scaled_fixed_c/_asm only ever look at
 * the pixel bytes (see their own signature), so the header before them
 * doesn't matter to those two, but keeping one shared buffer for all
 * three paths keeps this test honest (identical source bytes each time). */
static unsigned char g_test_sprite[2 + CINEMA_V2_WIDTH * CINEMA_V2_HEIGHT];

static void fill_test_pattern(void)
{
    unsigned i;
    gfx_sprite_t *s = (gfx_sprite_t *)g_test_sprite;

    s->width = CINEMA_V2_WIDTH;
    s->height = CINEMA_V2_HEIGHT;
    for (i = 0; i < (unsigned)(CINEMA_V2_WIDTH * CINEMA_V2_HEIGHT); ++i) {
        /* Deterministic, non-uniform (checkerboard-ish) pattern -- not
         * all-zero, so a broken renderer that e.g. only ever wrote zero
         * bytes wouldn't accidentally look identical to a correct one
         * if this were ever eyeballed on-screen. */
        s->data[i] = (unsigned char)(((i / CINEMA_V2_WIDTH) ^ i) & 0x0F);
    }
}

static void print_line(unsigned row, const char *text)
{
    gfx_SetTextXY(4, (int)(4 + row * 10));
    gfx_PrintString(text);
}

static void wait_for_key(void)
{
    while (os_GetCSC()) { /* drain any already-pressed key first */ }
    while (!os_GetCSC()) { /* the actual escape path */ }
}

int main(void)
{
    clock_t start, graphx_ticks;
#if CINEMA_RENDERER != CINEMA_RENDERER_GRAPHX
    clock_t candidate_ticks;
#endif
    unsigned i;
    char line[64];
    gfx_sprite_t *sprite = (gfx_sprite_t *)g_test_sprite;

    fill_test_pattern();

    gfx_Begin();
    gfx_SetDrawBuffer();
    gfx_SetTextFGColor(0);
    gfx_SetTextBGColor(15);
    gfx_FillScreen(15);

    print_line(0, "Cinema renderer benchmark");
    print_line(1, "(render-only, no USB/FAT)");
    gfx_SwapDraw();

    start = clock();
    for (i = 0; i < BENCH_ITERATIONS; ++i) {
        render_scaled_graphx((const struct gfx_sprite_t *)sprite);
    }
    graphx_ticks = clock() - start;

#if CINEMA_RENDERER == CINEMA_RENDERER_FIXED_C
    start = clock();
    for (i = 0; i < BENCH_ITERATIONS; ++i) {
        render_scaled_fixed_c(sprite->data);
    }
    candidate_ticks = clock() - start;
#elif CINEMA_RENDERER == CINEMA_RENDERER_FIXED_ASM
    start = clock();
    for (i = 0; i < BENCH_ITERATIONS; ++i) {
        render_scaled_fixed_asm(sprite->data);
    }
    candidate_ticks = clock() - start;
#endif

    gfx_SetDrawBuffer();
    gfx_FillScreen(15);
    print_line(0, "Cinema renderer benchmark");

    sprintf(line, "iterations: %d", BENCH_ITERATIONS);
    print_line(1, line);

    /* Routing proof (see render_v2.h): print which renderer this binary
     * was built with and how many times ITS OWN counter -- incremented
     * inside the renderer function itself, not inferred -- actually
     * fired. Must read BENCH_ITERATIONS for the active renderer and 0
     * for both others, or something upstream (wrong artifact run, wrong
     * build) is not what it looks like. */
    sprintf(line, "renderer: %s (id %d)", CINEMA_RENDERER_ACTIVE_NAME,
            CINEMA_RENDERER_ACTIVE_ID);
    print_line(2, line);
    sprintf(line, "calls graphx=%lu fixedc=%lu asm=%lu",
            (unsigned long)g_render_calls[CINEMA_RENDERER_ID_GRAPHX],
            (unsigned long)g_render_calls[CINEMA_RENDERER_ID_FIXED_C],
            (unsigned long)g_render_calls[CINEMA_RENDERER_ID_FIXED_ASM]);
    print_line(3, line);
    sprintf(line, "(active renderer's count should = %d)", BENCH_ITERATIONS);
    print_line(4, line);

    sprintf(line, "graphx total ticks: %lu", (unsigned long)graphx_ticks);
    print_line(5, line);
    sprintf(line, "graphx avg us: %lu",
            (unsigned long)(((uint64_t)graphx_ticks * 1000000u)
                             / ((uint64_t)BENCH_ITERATIONS * CLOCKS_PER_SEC)));
    print_line(6, line);

#if CINEMA_RENDERER != CINEMA_RENDERER_GRAPHX
    sprintf(line, "candidate total ticks: %lu", (unsigned long)candidate_ticks);
    print_line(7, line);
    sprintf(line, "candidate avg us: %lu",
            (unsigned long)(((uint64_t)candidate_ticks * 1000000u)
                             / ((uint64_t)BENCH_ITERATIONS * CLOCKS_PER_SEC)));
    print_line(8, line);
#else
    print_line(7, "(graphx-only build --");
    print_line(8, " no candidate to compare)");
#endif

    print_line(10, "press any key to exit");
    gfx_SwapDraw();

    wait_for_key();

    gfx_End();
    return 0;
}
