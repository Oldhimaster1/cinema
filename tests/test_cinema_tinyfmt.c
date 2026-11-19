#include "../src/cinema_tinyfmt.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK_FMT(fmt, ...) do { \
    char actual[256], expected[256]; \
    int ac = cinema_snprintf(actual, sizeof(actual), fmt, __VA_ARGS__); \
    int ex = snprintf(expected, sizeof(expected), fmt, __VA_ARGS__); \
    if (ac != ex || strcmp(actual, expected)) { \
        fprintf(stderr, "format mismatch '%s': '%s' (%d) != '%s' (%d)\n", \
                fmt, actual, ac, expected, ex); failures++; \
    } \
} while (0)

int main(void)
{
    unsigned i;
    CHECK_FMT("%s (%s)", "timeout", "frame read");
    CHECK_FMT("%c[D] %s", '>', "MOVIE.BIN");
    CHECK_FMT("%s/%s %u%% %s BAT%u", "1:02", "4:35", 125u, "PLAY", 3u);
    CHECK_FMT("%c Delay: %+ld ms", ' ', -175L);
    CHECK_FMT("F%lu.%lu D%lums DR%lu %u%% B%+d C%u",
              24UL, 1UL, 18UL, 0UL, 100u, -8, 110u);
    CHECK_FMT("R%08lX", 0x00ADCAB1UL);
    CHECK_FMT("CM%06lX", 0x00ABCDEFUL);
    CHECK_FMT("%02X%02X%02X", 0u, 15u, 255u);
    CHECK_FMT("%lu:%02lu:%02lu", 2UL, 7UL, 3UL);
    CHECK_FMT("%03lu", 0UL);
    CHECK_FMT("%03lu", 999UL);
    CHECK_FMT("%+ld", (long)INT32_MIN);
    CHECK_FMT("%+ld", (long)INT32_MAX);
    CHECK_FMT("%+d", INT_MIN);
    CHECK_FMT("%+d", INT_MAX);
    CHECK_FMT("%u", UINT_MAX);
    CHECK_FMT("%lu", 0xFFFFFFFFUL);
    CHECK_FMT("%% %c %s", 'A', "hello");
    for (i = 0; i < 10000u; ++i) {
        unsigned long n = (unsigned long)((uint32_t)rand() << 1u ^ (uint32_t)rand());
        unsigned w = (unsigned)(n % 1000u);
        CHECK_FMT("%lu.%03lu %u%%", n, n % 1000u, w);
        CHECK_FMT("%08lX", n);
    }
    {
        char a[4], b[4];
        size_t cap = (size_t)(3 + rand() % 2);
        int ac = cinema_snprintf(a, cap, "%s %u", "abcdefgh", 123u);
        int ex = snprintf(b, cap, "%s %u", "abcdefgh", 123u);
        if (ac != ex || strcmp(a, b)) failures++;
    }
    {
        char a[128], b[128];
        int ac = cinema_sprintf(a, "%c[%s] %s %s", '>', "BIN", "MOVIE", "0:12");
        int ex = sprintf(b, "%c[%s] %s %s", '>', "BIN", "MOVIE", "0:12");
        if (ac != ex || strcmp(a, b)) failures++;
    }
    if (failures) return 1;
    puts("small formatter matched libc on 20000+ integer/string cases");
    return 0;
}
