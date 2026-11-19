#include "../src/cin2.h"
#include "../src/fat32ro.h"
#include "../src/player_v2.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tice.h>
#include <graphx.h>

void sim_set_drive(const uint8_t *drive, uint32_t sector_count);
void sim_reset_injected_keys(void);
extern unsigned g_auto_asm_calls[4];
extern unsigned g_auto_raw_asm_calls;
extern uint8_t gfx_vram_stub[GFX_LCD_HEIGHT][GFX_LCD_WIDTH];
extern char g_auto_sub_status[100];

int main(void)
{
    enum { FRAMES = 12 };
    uint32_t sectors = 1 + FRAMES * CIN2_PACKED4_FRAME_SECTORS;
    uint8_t *drive = calloc(sectors + 1u, 512u);
    cin2_header_t header;
    fat32ro_extent_map_t movie_map;
    fat32ro_extent_map_t subtitle_map;
    global_t global;
    uint8_t subtitle_cache[512];
    uint8_t *csu;
    bool ok;
    uint32_t f;
    if (!drive) return 1;

    memset(&header, 0, sizeof(header));
    header.width = 160;
    header.height = 96;
    header.fps_num = 24;
    header.fps_den = 1;
    header.frame_count = FRAMES;
    header.flags = CIN2_FLAG_PACKED4;
    for (f = 0; f < 16; ++f) header.palette[f] = (uint16_t)(f * 0x1111);
    cin2_build_header(drive, &header);
    for (f = 0; f < FRAMES; ++f)
        memset(drive + (uint32_t)cin2_frame_lba_for(f, CIN2_FLAG_PACKED4) * 512u,
               (int)((f << 4) | f), 7680);

    csu = drive + sectors * 512u;
    memcpy(csu, "CSU1", 4); csu[4] = 1;
    csu[6] = 40; csu[8] = 72;
    csu[12] = 1;
    csu[20] = FRAMES;
    csu[24] = 24; csu[28] = 1;
    csu[44] = FRAMES;
    csu[48] = 1; csu[49] = 1; csu[52] = 'A';

    memset(&movie_map, 0, sizeof(movie_map));
    movie_map.extent_count = 1;
    movie_map.extents[0].lba = 0;
    movie_map.extents[0].sectors = sectors;
    movie_map.total_sectors = sectors;
    memset(&subtitle_map, 0, sizeof(subtitle_map));
    subtitle_map.extent_count = 1;
    subtitle_map.extents[0].lba = sectors;
    subtitle_map.extents[0].sectors = 1;
    subtitle_map.total_sectors = 1;

    memset(&global, 0, sizeof(global));
    global.usb = (usb_device_t)(uintptr_t)1;
    sim_set_drive(drive, sectors + 1u);
    sim_reset_injected_keys();
    player_v2_set_subtitle_stream(&subtitle_map, 112u, 0u, true, subtitle_cache);
    ok = player_v2_run(&global, &header, 0u, &movie_map, "");
    if (!ok || g_auto_asm_calls[0] != 17 || g_auto_asm_calls[1] != 17 ||
        g_auto_asm_calls[2] != 17 || g_auto_asm_calls[3] != 17 + FRAMES ||
        gfx_vram_stub[24][0] != FRAMES - 1 ||
        strcmp(g_auto_sub_status, "SUB DIFF GRAPHX") != 0) {
        fprintf(stderr, "FAIL: auto-test selection / P4 presentation: %d; "
                "calls %u %u %u %u; pixel %u; subtitle %s\n", ok,
                g_auto_asm_calls[0], g_auto_asm_calls[1],
                g_auto_asm_calls[2], g_auto_asm_calls[3],
                gfx_vram_stub[24][0], g_auto_sub_status);
        free(drive);
        return 1;
    }
    free(drive);

    /* RAW8 is the second format handled by the same installed .8xp.
     * GraphX is the reference; host fixed ASM is faster and pixel-identical.
     * This also catches state leaking between two separate movie launches. */
    sectors = 1u + FRAMES * CIN2_FRAME_SECTORS;
    drive = calloc(sectors, 512u);
    if (!drive) return 1;
    memset(&header, 0, sizeof(header));
    header.width = 160;
    header.height = 96;
    header.fps_num = 24;
    header.fps_den = 1;
    header.frame_count = FRAMES;
    cin2_build_header(drive, &header);
    for (f = 0; f < FRAMES; ++f)
        memset(drive + (uint32_t)cin2_frame_lba_for(f, 0u) * 512u,
               (int)f, 15360);
    movie_map.extents[0].sectors = sectors;
    movie_map.total_sectors = sectors;
    sim_set_drive(drive, sectors);
    ok = player_v2_run(&global, &header, 0u, &movie_map, "");
    if (!ok || g_auto_raw_asm_calls != 17u + FRAMES ||
        gfx_vram_stub[24][0] != FRAMES - 1) {
        fprintf(stderr, "FAIL: RAW8 auto-test selection: %d, ASM calls %u, pixel %u\n",
                ok, g_auto_raw_asm_calls, gfx_vram_stub[24][0]);
        free(drive);
        return 1;
    }
    free(drive);
    printf("one-install auto mode selected fastest correct renderers, "
           "rejected corrupted frames, and played %d Packed4 and RAW8 "
           "frames (with subtitle fallback)\n", FRAMES);
    return 0;
}
