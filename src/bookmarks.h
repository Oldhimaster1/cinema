#ifndef CINEMA_BOOKMARKS_H
#define CINEMA_BOOKMARKS_H
#include <stdint.h>
#include "cin2.h"
#define CINEMA_BOOKMARK_APPVAR "SSCBMK2"
#define CINEMA_BOOKMARK_MAX 8
typedef struct { uint32_t movie_key; uint8_t count; uint32_t frame[CINEMA_BOOKMARK_MAX]; } cinema_bookmarks_t;
uint32_t cinema_movie_key(const cin2_header_t *h);
void cinema_bookmarks_load(cinema_bookmarks_t *b,uint32_t key);
void cinema_bookmarks_save(const cinema_bookmarks_t *b);
void cinema_bookmarks_add(cinema_bookmarks_t *b,uint32_t frame);
#endif
