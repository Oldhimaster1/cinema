#include "render_autotest.h"

#include <string.h>

uint32_t cinema_autotest_hash(const uint8_t *bytes, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    size_t i;
    for (i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

cinema_autotest_result_t cinema_autotest_run(
    const cinema_autotest_draw_t *draws, uint8_t count,
    const uint8_t *frame, uint8_t *framebuffer, size_t framebuffer_bytes,
    cinema_autotest_clock_t clock_now, void *clock_context)
{
    cinema_autotest_result_t result = {{0}, {0}, 0, 0, 0};
    uint32_t best_ticks = 0;
    uint8_t candidate;

    if (!draws || !frame || !framebuffer || !clock_now || !framebuffer_bytes ||
        !count || count > CINEMA_AUTOTEST_MAX || !draws[0]) return result;

    result.count = count;
    for (candidate = 0; candidate < count; ++candidate) {
        uint8_t repeat;
        uint32_t before;
        if (!draws[candidate]) continue;

        /* Clearing and the full-buffer checksum are not included in the
         * render timing. Clearing avoids a stale pixel hiding an incomplete
         * renderer, and includes all LCD margins in the comparison. */
        /* The first GraphX call can pay a pending LCD swap wait; cached
         * subtitles also pay one-time glyph generation. Warm each path
         * once outside the measurement to compare sustained frame cost. */
        memset(framebuffer, 0, framebuffer_bytes);
        draws[candidate](frame);
        memset(framebuffer, 0, framebuffer_bytes);
        before = clock_now(clock_context);
        for (repeat = 0; repeat < CINEMA_AUTOTEST_REPEATS; ++repeat)
            draws[candidate](frame);
        result.ticks[candidate] = clock_now(clock_context) - before;
        result.hashes[candidate] = cinema_autotest_hash(framebuffer, framebuffer_bytes);

        if (candidate == 0) {
            result.valid_mask = 1u;
            best_ticks = result.ticks[0];
        } else if (result.hashes[candidate] == result.hashes[0]) {
            result.valid_mask |= (uint8_t)(1u << candidate);
            /* Require a >=7% advantage and a nonzero measured baseline
             * before risking an unproven alternative. Comparisons are
             * widened to avoid wraparound on long/paused clock sources. */
            if (best_ticks >= 16u && result.ticks[candidate] != 0u &&
                (uint64_t)result.ticks[candidate] * 100u <
                (uint64_t)best_ticks * 93u) {
                best_ticks = result.ticks[candidate];
                result.selected = candidate;
            }
        }
    }
    return result;
}
