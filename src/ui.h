#ifndef CINEMA_UI_H
#define CINEMA_UI_H
#include <stdint.h>
#include <stdbool.h>
#define CINEMA_SCALE_2X 0
#define CINEMA_SCALE_ORIGINAL 1
#define CINEMA_SCALE_STRETCH 2
void ui_fill_rect(uint8_t *fb, uint16_t stride, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t c);
void ui_draw_text(uint8_t *fb, uint16_t stride, uint16_t x, uint16_t y, const char *s, uint8_t c);
void ui_draw_progress(uint8_t *fb, uint16_t stride, uint32_t current, uint32_t total, uint8_t ready, uint8_t slots, bool paused, uint32_t fps_num, uint32_t fps_den);
void ui_format_time(char out[9], uint32_t frame, uint32_t fps_num, uint32_t fps_den);
#endif
