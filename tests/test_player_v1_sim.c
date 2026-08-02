/* End-to-end test of src/player_v1.c (the fixed legacy player) against
 * a synthetic in-memory 31-sector/frame drive, linked against
 * tests/stub_impl_v1_sim.c instead of real hardware. */
#include "../src/player_v1.h"
#include "../src/cinema.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define V1_SECTORS_PER_FRAME 31

extern void sim_set_drive(const uint8_t *drive, uint32_t sector_count);
extern void sim_exit_after_getcsc_calls(int n);
extern void sim_inject_read_failure_at_lba(uint32_t lba);
extern int sim_get_appvar(uint8_t *out, size_t out_size);
extern unsigned g_frames_rendered;

static int g_failures = 0;
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_failures++; \
        } \
    } while (0)

static uint8_t *build_synthetic_v1_drive(uint32_t frame_count, uint32_t *out_sectors)
{
    uint32_t sectors = frame_count * V1_SECTORS_PER_FRAME;
    uint8_t *drive = calloc((size_t)sectors, 512);
    uint32_t f;

    for (f = 0; f < frame_count; ++f) {
        memset(drive + (uint64_t)f * V1_SECTORS_PER_FRAME * 512,
               (int)(f & 0xFF), V1_SECTORS_PER_FRAME * 512);
    }

    *out_sectors = sectors;
    return drive;
}

static void test_clean_exit_and_resume_save(void)
{
    uint32_t sectors;
    uint8_t *drive = build_synthetic_v1_drive(6, &sectors);
    global_t global;
    bool ok;

    memset(&global, 0, sizeof(global));
    global.usb = (usb_device_t)(uintptr_t)1;

    sim_set_drive(drive, sectors);
    sim_exit_after_getcsc_calls(3); /* 2 full 2-frame iterations, then exit */
    sim_inject_read_failure_at_lba(0xFFFFFFFFu); /* no injected failure */
    g_frames_rendered = 0;

    ok = player_v1_run(&global, 0);

    CHECK(ok, "player_v1_run reports success on user-initiated exit");
    CHECK(g_frames_rendered == 4, "2 iterations x 2 frames each were rendered");

    {
        uint32_t saved_lba = 0xFFFFFFFFu;
        uint8_t raw[sizeof(uint32_t)];
        int len = sim_get_appvar(raw, sizeof(raw));

        CHECK(len == (int)sizeof(uint32_t), "v1 resume record has the original 4-byte size");
        if (len == (int)sizeof(uint32_t)) {
            memcpy(&saved_lba, raw, sizeof(uint32_t));
        }
        /* 1 initial palette read + 2 per iteration x 2 iterations = 5
         * palette reads total, each advancing lba by 31 sectors. */
        CHECK(saved_lba == 5 * V1_SECTORS_PER_FRAME,
              "saved LBA matches the original advance-by-31-per-frame semantics");
    }

    free(drive);
}

static void test_read_error_is_fatal_and_not_saved(void)
{
    uint32_t sectors;
    uint8_t *drive = build_synthetic_v1_drive(6, &sectors);
    global_t global;
    bool ok;

    memset(&global, 0, sizeof(global));
    global.usb = (usb_device_t)(uintptr_t)1;

    sim_set_drive(drive, sectors);
    sim_exit_after_getcsc_calls(-1); /* never a keypress -- only the error should stop it */
    /* Fail the image read for frame 2 (palette LBA 2*31=62, image LBA 63). */
    sim_inject_read_failure_at_lba(63);
    g_frames_rendered = 0;

    ok = player_v1_run(&global, 0);

    CHECK(!ok, "player_v1_run reports failure when a transfer errors");

    free(drive);
}

int main(void)
{
    test_clean_exit_and_resume_save();
    test_read_error_is_fatal_and_not_saved();

    if (g_failures == 0) {
        printf("All player_v1 simulation tests passed.\n");
        return 0;
    }

    printf("%d test(s) failed.\n", g_failures);
    return 1;
}
