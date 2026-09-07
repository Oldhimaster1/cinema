/* Focused integration check for CINEMA_RENDERER_FIXED_C: confirms real
 * playback through player_v2_loop() actually calls render_scaled_fixed_c()
 * and produces correct on-screen pixels -- not just that the algorithm is
 * correct in isolation (tests/test_render_v2.c already proves that) or
 * that playback merely "doesn't crash" with it selected. Deliberately a
 * separate, small file rather than reusing tests/test_player_v2_sim.c's
 * whole suite: that suite's assertions lean on g_frames_rendered, a
 * counter the mocked gfx_ScaledSprite_NoClip() stub increments -- which
 * this renderer never calls, since it writes pixels directly instead. */
#include "../src/cin2.h"
#include "../src/fat32ro.h"
#include "../src/player_v2.h"

#include <graphx.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* From tests/stub_impl_sim.c */
extern void sim_set_drive(const uint8_t *drive, uint32_t sector_count);

static int g_failures = 0;
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_failures++; \
        } \
    } while (0)

#define DST_Y0 ((GFX_LCD_HEIGHT - CINEMA_V2_DEST_HEIGHT) / 2) /* 24 */
#define DST_Y1 (DST_Y0 + CINEMA_V2_DEST_HEIGHT)               /* 216 */

static uint8_t *build_synthetic_drive(uint32_t frame_count, uint32_t *out_sectors,
                                        uint8_t *out_last_frame_value)
{
    uint32_t sectors = 1 + frame_count * CIN2_FRAME_SECTORS;
    uint8_t *drive = calloc((size_t)sectors, 512);
    cin2_header_t header;
    uint32_t f;
    int i;

    memset(&header, 0, sizeof(header));
    header.width = CINEMA_V2_WIDTH;
    header.height = CINEMA_V2_HEIGHT;
    header.fps_num = 24;
    header.fps_den = 1;
    header.frame_count = frame_count;
    for (i = 0; i < 16; ++i) {
        header.palette[i] = (uint16_t)(i * 0x1111);
    }
    cin2_build_header(drive, &header);

    for (f = 0; f < frame_count; ++f) {
        uint8_t *frame_bytes = drive + (uint64_t)cin2_frame_lba(f) * 512;
        uint8_t value = (uint8_t)(f + 1); /* distinctive per-frame value, never 0 */

        memset(frame_bytes, value, CINEMA_V2_WIDTH * CINEMA_V2_HEIGHT);
        if (f == frame_count - 1) {
            *out_last_frame_value = value;
        }
    }

    *out_sectors = sectors;
    return drive;
}

static fat32ro_extent_map_t identity_map(uint32_t sectors)
{
    fat32ro_extent_map_t map;

    memset(&map, 0, sizeof(map));
    map.extent_count = 1;
    map.extents[0].lba = 0;
    map.extents[0].sectors = sectors;
    map.total_sectors = sectors;
    return map;
}

int main(void)
{
    const uint32_t frame_count = 10;
    uint32_t sectors;
    uint8_t last_frame_value = 0;
    uint8_t *drive = build_synthetic_drive(frame_count, &sectors, &last_frame_value);
    global_t global;
    cin2_header_t header;
    bool ok;
    unsigned x, y;
    int video_region_ok = 1;
    int margins_untouched = 1;

    memset(&global, 0, sizeof(global));
    global.usb = (usb_device_t)(uintptr_t)1;

    memset(gfx_vram_stub, 0xAA, sizeof(gfx_vram_stub)); /* canary */

    sim_set_drive(drive, sectors);
    CHECK(cin2_parse_header(drive, &header), "synthetic header parses");
    {
        fat32ro_extent_map_t map = identity_map(sectors);

        ok = player_v2_run(&global, &header, 0, &map, "");
    }
    CHECK(ok, "playback with CINEMA_RENDERER_FIXED_C reports success");

    /* The last frame presented should still be on screen (nothing
     * re-renders after the final frame): every pixel in the video
     * region must equal that frame's distinctive byte value. */
    for (y = DST_Y0; y < DST_Y1 && video_region_ok; ++y) {
        for (x = 0; x < GFX_LCD_WIDTH; ++x) {
            if (gfx_vram_stub[y][x] != last_frame_value) {
                video_region_ok = 0;
                break;
            }
        }
    }
    CHECK(video_region_ok,
          "fixed-C renderer actually drew the last frame's pixels into the video region");

    for (y = 0; y < DST_Y0 && margins_untouched; ++y) {
        for (x = 0; x < GFX_LCD_WIDTH; ++x) {
            if (gfx_vram_stub[y][x] != 0xAA) { margins_untouched = 0; break; }
        }
    }
    for (y = DST_Y1; y < GFX_LCD_HEIGHT && margins_untouched; ++y) {
        for (x = 0; x < GFX_LCD_WIDTH; ++x) {
            if (gfx_vram_stub[y][x] != 0xAA) { margins_untouched = 0; break; }
        }
    }
    CHECK(margins_untouched,
          "fixed-C renderer left the letterbox margin (rows outside 24..215) untouched");

    free(drive);

    if (g_failures == 0) {
        printf("player_v2 fixed-C renderer integration test passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", g_failures);
    return 1;
}
