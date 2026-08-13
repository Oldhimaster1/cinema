/* Stub of the real CE-toolchain graphx.h -- see ce_types.h header
 * comment. gfx_vbuffer's shape (a real 2D array lvalue, not a flat
 * pointer) and all signatures below are transcribed from
 * https://github.com/CE-Programming/toolchain src/graphx/graphx.h;
 * getting gfx_vbuffer's exact shape right here matters because
 * src/player_v2.c and src/decode.c code against it directly instead of
 * going through gfx_Sprite/GraphX's scaler. */
#ifndef CINEMA_TEST_GRAPHX_H
#define CINEMA_TEST_GRAPHX_H

#include "ce_types.h"
#include <stdint.h>
#include <stdlib.h>

#define GFX_LCD_WIDTH  320
#define GFX_LCD_HEIGHT 240

typedef struct gfx_sprite_t {
    uint8_t width;
    uint8_t height;
    uint8_t data[];
} gfx_sprite_t;

extern uint8_t gfx_vram_stub[GFX_LCD_HEIGHT][GFX_LCD_WIDTH];
#define gfx_vbuffer gfx_vram_stub

void gfx_Begin(void);
void gfx_End(void);
void gfx_SwapDraw(void);
void gfx_Wait(void);

typedef uint8_t gfx_buffer_t; /* opaque, matches upstream's internal enum */
#define gfx_buffer ((gfx_buffer_t)1)
void gfx_SetDraw(gfx_buffer_t mode);
#define gfx_SetDrawBuffer() gfx_SetDraw(gfx_buffer)

void gfx_ZeroScreen(void);
void gfx_SetPalette(const void *palette, uint24_t size, uint8_t offset);

gfx_sprite_t *gfx_AllocSprite(uint8_t width, uint8_t height,
                               void *(*alloc_routine)(size_t));
#define gfx_MallocSprite(width, height) gfx_AllocSprite(width, height, malloc)

void gfx_ScaledSprite_NoClip(const gfx_sprite_t *sprite, uint24_t x,
                              uint8_t y, uint8_t width_scale,
                              uint8_t height_scale);

#define gfx_screen ((gfx_buffer_t)0)
#define gfx_SetDrawScreen() gfx_SetDraw(gfx_screen)

uint8_t gfx_SetColor(uint8_t index);
void gfx_FillRectangle_NoClip(uint24_t x, uint8_t y, uint24_t width,
                               uint8_t height);
uint8_t gfx_SetTextFGColor(uint8_t color);
uint8_t gfx_SetTextBGColor(uint8_t color);
void gfx_PrintStringXY(const char *string, int x, int y);

#endif
