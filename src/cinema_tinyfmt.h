#ifndef CINEMA_TINYFMT_H
#define CINEMA_TINYFMT_H

#include <stddef.h>

/* Only the integer/string formats used by Cinema's on-screen messages.
 * It is deliberately opted into by the compact hardware test build; the
 * full production build continues to use the normal libc formatter. */
int cinema_snprintf(char *buffer, size_t size, const char *format, ...);
int cinema_sprintf(char *buffer, const char *format, ...);

#endif
