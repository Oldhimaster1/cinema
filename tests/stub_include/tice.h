/* Stub of the real CE-toolchain tice.h -- see ce_types.h header
 * comment. Key-code values transcribed from
 * https://github.com/CE-Programming/toolchain src/ce/include/ti/getcsc.h.
 * clock()/CLOCKS_PER_SEC come from the real toolchain's time.h (libc
 * port); we just use the host's <time.h>, which is structurally
 * equivalent (clock_t, CLOCKS_PER_SEC, clock()) even though the tick
 * rate differs from the real 32768 Hz. */
#ifndef CINEMA_TEST_TICE_H
#define CINEMA_TEST_TICE_H

#include "ce_types.h"
#include <stdint.h>
#include <time.h>

#define sk_Down    0x01
#define sk_Left    0x02
#define sk_Right   0x03
#define sk_Up      0x04
#define sk_Enter   0x09
#define sk_Clear   0x0F
#define sk_0       0x21
#define sk_2nd     0x36
#define sk_Mode    0x37
#define sk_Del     0x38

void os_SetCursorPos(uint8_t row, uint8_t col);
void os_PutStrFull(char *str);
void os_NewLine(void);
void os_ClrHome(void);
uint8_t os_GetCSC(void);

/* Real declarations live in sys/power.h, pulled in transitively by the
 * real tice.h; main.c calls these to run at full CPU speed instead of
 * the OS's power-saving default. */
void boot_Set6MHzMode(void);
void boot_Set48MHzMode(void);

#endif
