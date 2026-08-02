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


void cinema_draw_packed4_original(const uint8_t *packed,uint8_t *fb,uint16_t stride)
{
    uint16_t y,x; uint8_t *row;
    for(y=0;y<CINEMA_V2_HEIGHT;y++){row=fb+(uint32_t)(72+y)*stride+80;for(x=0;x<CINEMA_V2_WIDTH/2;x++){uint8_t p=*packed++;*row++=p>>4;*row++=p&15;}}
}
void cinema_draw_packed4_stretch(const uint8_t *packed,uint8_t *fb,uint16_t stride)
{
    uint16_t dy,x; for(dy=0;dy<240;dy++){uint16_t sy=(uint16_t)(((uint32_t)dy*96)/240);const uint8_t *src=packed+(uint32_t)sy*80;uint8_t *row=fb+(uint32_t)dy*stride;for(x=0;x<80;x++){uint8_t p=*src++;uint8_t a=p>>4,b=p&15;*row++=a;*row++=a;*row++=b;*row++=b;}}
}
