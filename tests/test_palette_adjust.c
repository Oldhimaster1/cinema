#include "palette_adjust.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint16_t source[] = {0, 0x7FFF, 0x4210, 0x8000, 0x8421};
    uint16_t adjusted[5];
    uint16_t i;
    for (i = 0; i < 32768u; ++i) {
        assert(cinema_adjust_rgb1555(i, 0, 100) == i);
        assert(cinema_adjust_rgb1555(i | 0x8000u, 0, 100) == (i | 0x8000u));
        assert(cinema_adjust_rgb1555(i, -8, 100) <= i);
        assert(cinema_adjust_rgb1555(i, 8, 100) >= i);
    }
    cinema_adjust_palette(adjusted, source, 5, 0, 100);
    for (i = 0; i < 5; ++i) assert(source[i] == adjusted[i]);
    cinema_adjust_palette(adjusted, source, 5, 8, 150);
    assert(adjusted[1] == 0x7FFF);
    assert((adjusted[3] & 0x8000) != 0);
    puts("palette brightness/contrast exhaustive RGB1555 test passed");
    return 0;
}
