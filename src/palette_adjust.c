#include "palette_adjust.h"

static uint16_t adjust_channel(uint16_t channel, int8_t brightness, uint8_t contrast)
{
    int32_t value = 15 + ((int32_t)channel - 15) * contrast / 100 + brightness;
    if (value < 0) value = 0;
    if (value > 31) value = 31;
    return (uint16_t)value;
}

uint16_t cinema_adjust_rgb1555(uint16_t color, int8_t brightness, uint8_t contrast)
{
    return (uint16_t)((color & 0x8000u)
        | (adjust_channel((color >> 10) & 31u, brightness, contrast) << 10)
        | (adjust_channel((color >> 5) & 31u, brightness, contrast) << 5)
        | adjust_channel(color & 31u, brightness, contrast));
}

void cinema_adjust_palette(uint16_t *out, const uint16_t *base,
                           uint8_t entries, int8_t brightness, uint8_t contrast)
{
    uint8_t i;
    for (i = 0; i < entries; ++i)
        out[i] = cinema_adjust_rgb1555(base[i], brightness, contrast);
}
