/* Stub of the real CE-toolchain fileioc.h -- see ce_types.h header
 * comment. Signatures transcribed from
 * https://github.com/CE-Programming/toolchain src/fileioc/fileioc.h. */
#ifndef CINEMA_TEST_FILEIOC_H
#define CINEMA_TEST_FILEIOC_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t ti_var_t;

uint8_t ti_Open(const char *name, const char *mode);
int ti_Close(uint8_t handle);
size_t ti_Read(void *data, size_t size, size_t count, uint8_t handle);
size_t ti_Write(const void *data, size_t size, size_t count, uint8_t handle);
int ti_SetArchiveStatus(uint8_t archive, uint8_t handle);
void ti_SetGCBehavior(void (*before)(void), void (*after)(void));
int ti_Delete(const char *name);
int ti_Seek(int offset, unsigned int origin, uint8_t handle);

#endif
