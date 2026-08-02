/* NOT part of the shipped calculator build. Minimal stand-ins for
 * CE-toolchain-only types so the sources under src/ can be compiled
 * with a host compiler for structural validation (undeclared
 * identifiers, wrong signatures, type mismatches, missing includes)
 * since no ez80 CE toolchain is available in this environment. See
 * tests/README.md. */
#ifndef CINEMA_TEST_CE_TYPES_H
#define CINEMA_TEST_CE_TYPES_H

#include <stdint.h>

typedef uint32_t uint24_t;

#endif
