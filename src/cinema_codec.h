#ifndef CINEMA_CODEC_H
#define CINEMA_CODEC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
uint16_t cinema_rd16(const uint8_t *p); uint32_t cinema_rd32(const uint8_t *p);
void cinema_wr16(uint8_t *p,uint16_t v); void cinema_wr32(uint8_t *p,uint32_t v);
uint32_t cinema_crc32_zeroed(const uint8_t *data,size_t size,size_t zero_off,size_t zero_len);
#endif
