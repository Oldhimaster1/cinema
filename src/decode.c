#include "decode.h"

void cinema_draw_packed4_scaled2x(const uint8_t *packed,
                                   uint8_t *framebuffer,
                                   uint16_t framebuffer_stride,
                                   uint16_t y_offset)
{
    uint16_t y;

    for (y = 0; y < CINEMA_V2_HEIGHT; ++y) {
        uint8_t *row0 = framebuffer
            + (uint32_t)(y_offset + y * 2) * framebuffer_stride;
        uint8_t *row1 = row0 + framebuffer_stride;
        uint16_t pair;

        for (pair = 0; pair < CINEMA_V2_WIDTH / 2; ++pair) {
            uint8_t src = *packed++;
            uint8_t left = src >> 4;
            uint8_t right = src & 0x0F;
            uint16_t x = pair * 4;

            row0[x + 0] = left;
            row0[x + 1] = left;
            row0[x + 2] = right;
            row0[x + 3] = right;

            row1[x + 0] = left;
            row1[x + 1] = left;
            row1[x + 2] = right;
            row1[x + 3] = right;
        }
    }
}
