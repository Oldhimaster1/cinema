/* Pixel-correctness and guard-region tests for src/render_v2.c's fixed
 * 160x96 -> 320x192 2x scale-up, run on the host against the stub
 * graphx.h's real gfx_vram_stub array (tests/stub_include/graphx.h) --
 * no calculator or USB stack needed, since render_v2.c only touches
 * GraphX's draw-buffer macro and pure compile-time constants. */
#include "../src/player_v2.h"
#include "../src/render_v2.h"

#include <graphx.h>
#include <stdio.h>
#include <string.h>

/* stub_include/graphx.h declares gfx_vram_stub extern; this test is
 * self-contained (no other stub_impl*.c needed), so it supplies the
 * actual storage itself. */
uint8_t gfx_vram_stub[GFX_LCD_HEIGHT][GFX_LCD_WIDTH];

static int g_failures = 0;
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_failures++; \
        } \
    } while (0)

#define SRC_W CINEMA_V2_WIDTH  /* 160 */
#define SRC_H CINEMA_V2_HEIGHT /* 96 */
#define DST_Y0 ((GFX_LCD_HEIGHT - CINEMA_V2_DEST_HEIGHT) / 2) /* 24 */
#define DST_Y1 (DST_Y0 + CINEMA_V2_DEST_HEIGHT)               /* 216 */

#define GUARD_BYTES 16
#define GUARD_FILL 0x5A

/* Source buffer with canary padding on both sides, so an out-of-bounds
 * read past either end of the real 160x96 region is detectable. */
typedef struct {
    unsigned char before[GUARD_BYTES];
    unsigned char pixels[SRC_H * SRC_W];
    unsigned char after[GUARD_BYTES];
} guarded_src_t;

/* Deliberately naive, obviously-correct reference: direct 2D indexing,
 * explicit multiplication, no cleverness, no shared code with
 * render_scaled_fixed_c -- exactly the kind of independent ground truth
 * a "fixed C reference" needs to be checked against. */
static void naive_reference(const unsigned char *src, unsigned char dst[GFX_LCD_HEIGHT][GFX_LCD_WIDTH])
{
    unsigned y, x;

    for (y = 0; y < SRC_H; ++y) {
        for (x = 0; x < SRC_W; ++x) {
            unsigned char p = src[y * SRC_W + x];
            unsigned dy, dx;

            for (dy = 0; dy < 2; ++dy) {
                for (dx = 0; dx < 2; ++dx) {
                    dst[DST_Y0 + y * 2 + dy][x * 2 + dx] = p;
                }
            }
        }
    }
}

static void fill_pattern(unsigned char *pixels, int pattern)
{
    unsigned y, x;

    for (y = 0; y < SRC_H; ++y) {
        for (x = 0; x < SRC_W; ++x) {
            unsigned char v;

            switch (pattern) {
                case 0: v = 0; break;                       /* all-zero */
                case 1: v = 15; break;                       /* all max index */
                case 2: v = (unsigned char)(x & 0x0F); break; /* horizontal gradient */
                case 3: v = (unsigned char)(y & 0x0F); break; /* vertical gradient */
                case 4: v = (unsigned char)(((x + y) & 1) ? 0 : 15); break; /* checkerboard */
                case 5: v = (unsigned char)(x & 1); break;    /* alternating pixels */
                case 6: v = (unsigned char)(y & 1); break;    /* alternating rows */
                case 7: /* single-pixel impulses at all four corners */
                    v = 0;
                    if ((y == 0 && x == 0) || (y == 0 && x == SRC_W - 1)
                        || (y == SRC_H - 1 && x == 0) || (y == SRC_H - 1 && x == SRC_W - 1)) {
                        v = 15;
                    }
                    break;
                case 8: /* impulses near row boundaries (first/last row, mid columns) */
                    v = 0;
                    if (y == 0 || y == 1 || y == SRC_H - 2 || y == SRC_H - 1) {
                        v = (unsigned char)((x % 3 == 0) ? 7 : 0);
                    }
                    break;
                default: /* deterministic pseudo-random */
                {
                    unsigned seed = y * 1103515245u + x * 12345u + 7u;
                    v = (unsigned char)((seed >> 5) & 0xFF);
                    break;
                }
            }
            pixels[y * SRC_W + x] = v;
        }
    }
}

static void run_pattern_test(int pattern, const char *name)
{
    static guarded_src_t src;
    static unsigned char expected[GFX_LCD_HEIGHT][GFX_LCD_WIDTH];
    unsigned char src_before_snapshot[GUARD_BYTES];
    unsigned char src_after_snapshot[GUARD_BYTES];
    unsigned char src_pixels_snapshot[SRC_H * SRC_W];
    unsigned y;
    char msg[128];

    memset(&src, GUARD_FILL, sizeof(src));
    fill_pattern(src.pixels, pattern);
    memcpy(src_before_snapshot, src.before, GUARD_BYTES);
    memcpy(src_after_snapshot, src.after, GUARD_BYTES);
    memcpy(src_pixels_snapshot, src.pixels, sizeof(src_pixels_snapshot));

    memset(expected, GUARD_FILL, sizeof(expected));
    naive_reference(src.pixels, expected);

    memset(gfx_vram_stub, GUARD_FILL, sizeof(gfx_vram_stub));
    render_scaled_fixed_c(src.pixels);

    /* Guard rows above/below the video region must be untouched. */
    for (y = 0; y < DST_Y0; ++y) {
        snprintf(msg, sizeof(msg), "%s: row %u above video region untouched", name, y);
        CHECK(memcmp(gfx_vram_stub[y], expected[y], GFX_LCD_WIDTH) == 0, msg);
    }
    for (y = DST_Y1; y < GFX_LCD_HEIGHT; ++y) {
        snprintf(msg, sizeof(msg), "%s: row %u below video region untouched", name, y);
        CHECK(memcmp(gfx_vram_stub[y], expected[y], GFX_LCD_WIDTH) == 0, msg);
    }

    /* Every pixel in the 320x192 video region must match the naive
     * reference exactly. */
    for (y = DST_Y0; y < DST_Y1; ++y) {
        snprintf(msg, sizeof(msg), "%s: video row %u matches naive reference", name, y);
        CHECK(memcmp(gfx_vram_stub[y], expected[y], GFX_LCD_WIDTH) == 0, msg);
    }

    /* The source buffer (including its guard bytes) must be unchanged --
     * this is a read-only operation on src. */
    snprintf(msg, sizeof(msg), "%s: source guard bytes before the frame untouched", name);
    CHECK(memcmp(src.before, src_before_snapshot, GUARD_BYTES) == 0, msg);
    snprintf(msg, sizeof(msg), "%s: source guard bytes after the frame untouched", name);
    CHECK(memcmp(src.after, src_after_snapshot, GUARD_BYTES) == 0, msg);
    snprintf(msg, sizeof(msg), "%s: source pixel data itself unmodified", name);
    CHECK(memcmp(src.pixels, src_pixels_snapshot, sizeof(src_pixels_snapshot)) == 0, msg);
}

/* Explicit spot checks beyond the full-buffer comparison above: pin down
 * the exact corner/boundary behavior called out in the renderer phase's
 * requirements, independent of naive_reference's own correctness. */
static void test_exact_corners_and_boundaries(void)
{
    static guarded_src_t src;

    memset(&src, GUARD_FILL, sizeof(src));
    memset(src.pixels, 0, sizeof(src.pixels));
    src.pixels[0] = 9;                                  /* (x=0,   y=0)  */
    src.pixels[SRC_W - 1] = 10;                          /* (x=159, y=0)  */
    src.pixels[(SRC_H - 1) * SRC_W] = 11;                 /* (x=0,   y=95) */
    src.pixels[(SRC_H - 1) * SRC_W + SRC_W - 1] = 12;      /* (x=159, y=95) */

    memset(gfx_vram_stub, GUARD_FILL, sizeof(gfx_vram_stub));
    render_scaled_fixed_c(src.pixels);

    CHECK(gfx_vram_stub[DST_Y0][0] == 9 && gfx_vram_stub[DST_Y0][1] == 9
              && gfx_vram_stub[DST_Y0 + 1][0] == 9 && gfx_vram_stub[DST_Y0 + 1][1] == 9,
          "top-left source pixel becomes an exact 2x2 block at (0, row 24)");
    CHECK(gfx_vram_stub[DST_Y0][318] == 10 && gfx_vram_stub[DST_Y0][319] == 10
              && gfx_vram_stub[DST_Y0 + 1][318] == 10 && gfx_vram_stub[DST_Y0 + 1][319] == 10,
          "top-right source pixel becomes an exact 2x2 block ending at column 319");
    CHECK(gfx_vram_stub[DST_Y1 - 2][0] == 11 && gfx_vram_stub[DST_Y1 - 1][0] == 11,
          "bottom-left source pixel's 2x2 block ends exactly at row 215 (DST_Y1 - 1)");
    CHECK(gfx_vram_stub[DST_Y1 - 2][319] == 12 && gfx_vram_stub[DST_Y1 - 1][319] == 12,
          "bottom-right source pixel's 2x2 block lands at the very last row/column");
    CHECK(DST_Y0 == 24, "row 24 is where the video region begins (sanity check on the constant itself)");
    CHECK(DST_Y1 == 216, "row 215 is where the video region ends (sanity check on the constant itself)");
}

/* No row boundary is shared between two different source rows' 2x2
 * blocks -- i.e. row pairs don't overlap or bleed into each other. */
static void test_row_pairs_dont_overlap(void)
{
    static guarded_src_t src;
    unsigned y;

    memset(&src, GUARD_FILL, sizeof(src));
    /* Every source row gets its own distinct value, so any bleed between
     * row-pairs (e.g. writing row N's data into row N+1's destination)
     * shows up as a mismatched value rather than coincidentally correct
     * output. */
    for (y = 0; y < SRC_H; ++y) {
        memset(src.pixels + y * SRC_W, (int)(y % 16), SRC_W);
    }

    memset(gfx_vram_stub, GUARD_FILL, sizeof(gfx_vram_stub));
    render_scaled_fixed_c(src.pixels);

    for (y = 0; y < SRC_H; ++y) {
        unsigned char expected_value = (unsigned char)(y % 16);
        char msg[96];
        unsigned x_ok0 = 1, x_ok1 = 1;
        unsigned x;

        for (x = 0; x < GFX_LCD_WIDTH; ++x) {
            if (gfx_vram_stub[DST_Y0 + y * 2][x] != expected_value) x_ok0 = 0;
            if (gfx_vram_stub[DST_Y0 + y * 2 + 1][x] != expected_value) x_ok1 = 0;
        }
        snprintf(msg, sizeof(msg), "source row %u's first destination row is uniformly its own value", y);
        CHECK(x_ok0, msg);
        snprintf(msg, sizeof(msg), "source row %u's second destination row is uniformly its own value", y);
        CHECK(x_ok1, msg);
    }
}

int main(void)
{
    run_pattern_test(0, "all-zero frame");
    run_pattern_test(1, "all max index (15) frame");
    run_pattern_test(2, "horizontal gradient");
    run_pattern_test(3, "vertical gradient");
    run_pattern_test(4, "checkerboard");
    run_pattern_test(5, "alternating pixels");
    run_pattern_test(6, "alternating rows");
    run_pattern_test(7, "corner impulses");
    run_pattern_test(8, "row-boundary impulses");
    run_pattern_test(9, "deterministic pseudo-random");

    test_exact_corners_and_boundaries();
    test_row_pairs_dont_overlap();

    if (g_failures == 0) {
        printf("All render_v2 tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", g_failures);
    return 1;
}
