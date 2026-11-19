#include "../src/render_autotest.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t buffer[128];
static uint32_t ticks;
static uint32_t charges[4];
static uint8_t current;

static uint32_t now(void *unused)
{
    (void)unused;
    return ticks;
}
static void draw0(const uint8_t *frame)
{
    memset(buffer + 16, frame[0], 96);
    current = 0;
    ticks += charges[0];
}
static void draw1(const uint8_t *frame)
{
    draw0(frame);
    current = 1;
    ticks += charges[1] - charges[0];
}
static void draw2(const uint8_t *frame)
{
    draw0(frame);
    current = 2;
    buffer[55] = (uint8_t)(frame[0] ^ 1u); /* faster but corrupted */
    ticks += charges[2] - charges[0];
}
static void draw3(const uint8_t *frame)
{
    draw0(frame);
    current = 3;
    ticks += charges[3] - charges[0];
}

int main(void)
{
    const cinema_autotest_draw_t fns[4] = {draw0, draw1, draw2, draw3};
    const uint8_t input[1] = {19};
    cinema_autotest_result_t r;
    charges[0] = 100; charges[1] = 75; charges[2] = 20; charges[3] = 60;
    r = cinema_autotest_run(fns, 4, input, buffer, sizeof(buffer), now, NULL);
    assert(r.count == 4 && r.selected == 3);
    assert(r.valid_mask == (1u | 2u | 8u));
    assert(r.ticks[0] == 1600 && r.ticks[1] == 1200 && r.ticks[2] == 320);
    assert(r.ticks[3] == 960);
    assert(r.hashes[0] == r.hashes[1] && r.hashes[0] != r.hashes[2]);
    /* Null or absent inputs must safely select nothing. */
    r = cinema_autotest_run(NULL, 4, input, buffer, sizeof(buffer), now, NULL);
    assert(r.count == 0);
    r = cinema_autotest_run(fns, 5, input, buffer, sizeof(buffer), now, NULL);
    assert(r.count == 0);
    /* Near ties aren't trusted, even if the alternative is technically
     * faster in a tiny/unstable sample. */
    charges[0] = 100; charges[1] = 97; charges[2] = 10; charges[3] = 98;
    r = cinema_autotest_run(fns, 4, input, buffer, sizeof(buffer), now, NULL);
    assert(r.selected == 0);
    /* Bad clock resolution: never optimize on a zero-tick measurement. */
    charges[0] = 0; charges[1] = 0; charges[2] = 0; charges[3] = 0;
    r = cinema_autotest_run(fns, 4, input, buffer, sizeof(buffer), now, NULL);
    assert(r.selected == 0 && r.ticks[0] == 0);
    printf("automatic renderer selection and pixel mismatch tests passed\n");
    return 0;
}
