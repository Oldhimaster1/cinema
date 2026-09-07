/* Stub of the real CE-toolchain keypadc.h -- see ce_types.h header
 * comment. src/player_v2.c uses kb_Scan()/kb_Data for hold-to-scrub
 * seeking: unlike os_GetCSC() (tice.h), which only ever reports one
 * debounced press per physical press-and-release, kb_Data reflects raw,
 * continuously-held key state. Only the row/bits Cinema actually reads
 * (row 7: the arrow keys) are modeled here. */
#ifndef CINEMA_TEST_KEYPADC_H
#define CINEMA_TEST_KEYPADC_H

#include <stdint.h>

extern uint8_t kb_Data[8];

void kb_Scan(void);

#define kb_Down  (1 << 0)
#define kb_Left  (1 << 1)
#define kb_Right (1 << 2)
#define kb_Up    (1 << 3)

#endif
