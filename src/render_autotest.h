#ifndef CINEMA_RENDER_AUTOTEST_H
#define CINEMA_RENDER_AUTOTEST_H

#include <stddef.h>
#include <stdint.h>

/* An in-place, allocation-free renderer shootout. Every candidate draws
 * exactly the same decoded frame into the same off-screen buffer. Candidate
 * 0 is the known baseline; alternatives that differ by even one output byte
 * are never selected, regardless of speed. */
#define CINEMA_AUTOTEST_MAX 4u
#define CINEMA_AUTOTEST_REPEATS 16u

typedef void (*cinema_autotest_draw_t)(const uint8_t *frame);
typedef uint32_t (*cinema_autotest_clock_t)(void *context);

typedef struct {
    uint32_t ticks[CINEMA_AUTOTEST_MAX];
    uint32_t hashes[CINEMA_AUTOTEST_MAX];
    uint8_t count;
    uint8_t valid_mask;
    uint8_t selected;
} cinema_autotest_result_t;

uint32_t cinema_autotest_hash(const uint8_t *bytes, size_t length);

cinema_autotest_result_t cinema_autotest_run(
    const cinema_autotest_draw_t *draws, uint8_t count,
    const uint8_t *frame, uint8_t *framebuffer, size_t framebuffer_bytes,
    cinema_autotest_clock_t clock_now, void *clock_context);

#endif
