#ifndef CINEMA_PALETTE_ADJUST_H
#define CINEMA_PALETTE_ADJUST_H
#include <stdint.h>

/* Pure integer 5-bit palette adjustment. Neutral parameters are bit-exact. */
uint16_t cinema_adjust_rgb1555(uint16_t color, int8_t brightness, uint8_t contrast);
void cinema_adjust_palette(uint16_t *out, const uint16_t *base,
                           uint8_t entries, int8_t brightness, uint8_t contrast);
#endif
