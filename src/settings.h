#ifndef CINEMA_SETTINGS_H
#define CINEMA_SETTINGS_H
#include <stdint.h>
#define CINEMA_SETTINGS_APPVAR "SSCSET2"
typedef struct { uint8_t seek_index; uint8_t dashboard_seconds; uint8_t scale_mode; uint8_t auto_resume; } cinema_settings_t;
void cinema_settings_defaults(cinema_settings_t *s);
void cinema_settings_load(cinema_settings_t *s);
void cinema_settings_save(const cinema_settings_t *s);
uint16_t cinema_seek_seconds(const cinema_settings_t *s);
#endif
