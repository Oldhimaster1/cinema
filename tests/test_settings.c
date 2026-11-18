#include "cinema_settings.h"
#include "cinema_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t data[CINEMA_SETTINGS_BYTES];
    cinema_settings_t current, restored;
    uint8_t i;
    cinema_settings_defaults(&current);
    cinema_settings_encode(data, &current);
    assert(cinema_settings_decode(&restored, data, sizeof(data)));
    assert(cinema_settings_equal(&current, &restored));
    /* All 32 combinations of the existing four options + new monitor. */
    for (i = 0; i < 32; ++i) {
        current.flags = i;
        cinema_settings_encode(data, &current);
        assert(cinema_settings_decode(&restored, data, sizeof(data)));
        assert(cinema_settings_equal(&restored, &current));
    }
    /* Old CST1 stores are still decoded. Their monitor bit is off. */
    current.flags = 15;
    cinema_settings_encode(data, &current);
    assert(cinema_settings_decode(&restored, data, sizeof(data)));
    assert(!(restored.flags & CINEMA_SETTING_PERF_MONITOR));
    data[5] |= 0x80;
    cinema_wr32(data + 12, cinema_crc32_zeroed(data, sizeof(data), 12, 4));
    assert(!cinema_settings_decode(&restored, data, sizeof(data)));
    assert(restored.flags == (CINEMA_SETTING_STATUS | CINEMA_SETTING_THUMBS |
                              CINEMA_SETTING_REMEMBER));
    puts("32 settings combinations + old CST1 + corruption passed");
    return 0;
}
