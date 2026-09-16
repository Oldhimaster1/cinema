#ifndef CINEMA_SETTINGS_H
#define CINEMA_SETTINGS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CINEMA_SETTINGS_BYTES 16u
#define CINEMA_SETTING_SUBTITLES 1u
#define CINEMA_SETTING_STATUS 2u
#define CINEMA_SETTING_THUMBS 4u
#define CINEMA_SETTING_REMEMBER 8u
typedef struct {uint8_t flags;} cinema_settings_t;
void cinema_settings_defaults(cinema_settings_t *s);
bool cinema_settings_decode(cinema_settings_t *s,const uint8_t *b,size_t n);
void cinema_settings_encode(uint8_t *b,const cinema_settings_t *s);
bool cinema_settings_equal(const cinema_settings_t *a,const cinema_settings_t *b);
#endif
