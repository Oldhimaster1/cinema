/* Host-side stand-ins for the eZ80 assembly candidates. These prove the
 * runtime routing and safe pixel-mismatch fallback, NOT assembly cycles. */
#include <graphx.h>

#include <stdint.h>
#include <time.h>

void sim_advance_clock(clock_t ticks);
extern uint8_t gfx_vram_stub[GFX_LCD_HEIGHT][GFX_LCD_WIDTH];
unsigned g_auto_asm_calls[4];
unsigned g_auto_raw_asm_calls;

void render_scaled_fixed_asm(const unsigned char *src)
{
    uint16_t y, x;
    g_auto_raw_asm_calls++;
    sim_advance_clock(70);
    for (y = 0; y < 96; ++y)
        for (x = 0; x < 160; ++x) {
            uint8_t value = src[(uint32_t)y * 160 + x];
            uint16_t yy = (uint16_t)(24u + y * 2u);
            uint16_t xx = (uint16_t)(x * 2u);
            gfx_vram_stub[yy][xx] = value;
            gfx_vram_stub[yy][xx + 1] = value;
            gfx_vram_stub[yy + 1][xx] = value;
            gfx_vram_stub[yy + 1][xx + 1] = value;
        }
}

static void packed4_draw(const unsigned char *src, unsigned mode, clock_t charge)
{
    uint16_t y, x;
    g_auto_asm_calls[mode]++;
    sim_advance_clock(charge);
    for (y = 0; y < 96; ++y)
        for (x = 0; x < 160; ++x) {
            uint8_t byte = src[(uint32_t)y * 80 + x / 2];
            uint8_t value = (uint8_t)((x & 1u) ? (byte & 15u) : (byte >> 4));
            uint16_t yy = (uint16_t)(24u + y * 2u);
            uint16_t xx = (uint16_t)(x * 2u);
            gfx_vram_stub[yy][xx] = value;
            gfx_vram_stub[yy][xx + 1] = value;
            gfx_vram_stub[yy + 1][xx] = value;
            gfx_vram_stub[yy + 1][xx + 1] = value;
        }
    if (mode == 2) gfx_vram_stub[24][0] ^= 1u; /* fast but subtly wrong */
}

void render_scaled_packed4_asm(const unsigned char *src)
{ packed4_draw(src, 0, 110); }
void render_scaled_packed4_dual_asm(const unsigned char *src)
{ packed4_draw(src, 1, 85); }
void render_scaled_packed4_unrolled_asm(const unsigned char *src)
{ packed4_draw(src, 2, 15); }
void render_scaled_packed4_lut_asm(const unsigned char *src)
{ packed4_draw(src, 3, 50); }
