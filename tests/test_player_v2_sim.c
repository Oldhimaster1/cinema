/* End-to-end test of src/player_v2.c's scheduler/slot state machine
 * against a synthetic in-memory CIN2 drive, linked against
 * tests/stub_impl_sim.c instead of real hardware. See that file's
 * header comment for how msd_ReadAsync/clock() are simulated. */
#include "../src/cin2.h"
#include "../src/player_v2.h"
#include "../src/decode.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* From tests/stub_impl_sim.c */
extern void sim_set_drive(const uint8_t *drive, uint32_t sector_count);
extern void sim_inject_read_failure_at_lba(uint32_t lba);
extern int sim_get_resume_record(uint8_t *out, size_t out_size);
extern unsigned g_async_reads;
extern unsigned g_frames_rendered;

static int g_failures = 0;
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_failures++; \
        } \
    } while (0)

static uint8_t *build_synthetic_drive(uint32_t frame_count, uint32_t *out_sectors)
{
    uint32_t sectors = 1 + frame_count * CIN2_FRAME_SECTORS;
    uint8_t *drive = calloc((size_t)sectors, 512);
    cin2_header_t header;
    uint32_t f;
    int i;

    memset(&header, 0, sizeof(header));
    header.width = CINEMA_V2_WIDTH;
    header.height = CINEMA_V2_HEIGHT;
    header.fps_num = 24;
    header.fps_den = 1;
    header.frame_count = frame_count;
    for (i = 0; i < 16; ++i) {
        header.palette[i] = (uint16_t)(i * 0x1111);
    }
    cin2_build_header(drive, &header);

    for (f = 0; f < frame_count; ++f) {
        uint8_t *frame_bytes = drive + (uint64_t)cin2_frame_lba(f) * 512;
        /* Distinctive, checkable-by-eye pattern: every byte in frame f
         * encodes f's low nibble twice. Content doesn't matter for this
         * test (only slot bookkeeping does), but a recognizable pattern
         * makes failures easier to debug than all-zero frames would. */
        memset(frame_bytes, (int)(((f & 0x0F) << 4) | (f & 0x0F)),
               CINEMA_V2_PACKED_BYTES);
    }

    *out_sectors = sectors;
    return drive;
}

static void test_full_playback_no_resume(void)
{
    const uint32_t frame_count = 50;
    uint32_t sectors;
    uint8_t *drive = build_synthetic_drive(frame_count, &sectors);
    global_t global;
    cin2_header_t header;
    bool ok;

    memset(&global, 0, sizeof(global));
    global.usb = (usb_device_t)(uintptr_t)1;

    sim_set_drive(drive, sectors);
    CHECK(cin2_parse_header(drive, &header), "synthetic header parses");

    g_frames_rendered = 0;
    ok = player_v2_run(&global, &header, 0);

    CHECK(ok, "player_v2_run reports success");
    CHECK(g_frames_rendered == frame_count, "every frame got rendered exactly once");

    {
        uint8_t raw[CIN2_RESUME_BYTES];
        int len = sim_get_resume_record(raw, sizeof(raw));
        cin2_resume_t resume;

        CHECK(len == CIN2_RESUME_BYTES, "resume record was written with the right size");
        CHECK(cin2_parse_resume_record(raw, &resume), "written resume record parses");
        CHECK(resume.frame_count == frame_count, "resume record frame_count matches movie");
        CHECK(resume.last_presented_frame == frame_count - 1,
              "resume record points at the last frame shown");
    }

    free(drive);
}

static void test_resume_starts_mid_movie(void)
{
    const uint32_t frame_count = 20;
    uint32_t sectors;
    uint8_t *drive = build_synthetic_drive(frame_count, &sectors);
    global_t global;
    cin2_header_t header;
    bool ok;

    memset(&global, 0, sizeof(global));
    global.usb = (usb_device_t)(uintptr_t)1;

    sim_set_drive(drive, sectors);
    CHECK(cin2_parse_header(drive, &header), "synthetic header parses");

    g_frames_rendered = 0;
    /* Start at frame 15 of 20, as if resuming. */
    ok = player_v2_run(&global, &header, 15);

    CHECK(ok, "resumed playback reports success");
    CHECK(g_frames_rendered == 5, "only the remaining 5 frames were rendered");

    free(drive);
}

static void test_read_error_is_fatal_and_reported(void)
{
    const uint32_t frame_count = 30;
    uint32_t sectors;
    uint8_t *drive = build_synthetic_drive(frame_count, &sectors);
    global_t global;
    cin2_header_t header;
    bool ok;

    memset(&global, 0, sizeof(global));
    global.usb = (usb_device_t)(uintptr_t)1;

    sim_set_drive(drive, sectors);
    CHECK(cin2_parse_header(drive, &header), "synthetic header parses");
    /* Fail the read for frame 10's sectors (LBA of frame 10). */
    sim_inject_read_failure_at_lba(cin2_frame_lba(10));

    ok = player_v2_run(&global, &header, 0);

    CHECK(!ok, "player_v2_run reports failure when a frame read errors");

    free(drive);
}

static void test_single_frame_movie(void)
{
    uint32_t sectors;
    uint8_t *drive = build_synthetic_drive(1, &sectors);
    global_t global;
    cin2_header_t header;
    bool ok;

    memset(&global, 0, sizeof(global));
    global.usb = (usb_device_t)(uintptr_t)1;
    sim_set_drive(drive, sectors);
    CHECK(cin2_parse_header(drive, &header), "1-frame header parses");

    g_frames_rendered = 0;
    g_async_reads = 0;
    ok = player_v2_run(&global, &header, 0);

    CHECK(ok, "1-frame movie plays to completion");
    CHECK(g_frames_rendered == 1, "exactly 1 frame rendered");
    CHECK(g_async_reads == 1, "only 1 read ever queued (3 slots stay empty, never overfilled)");

    free(drive);
}

static void test_exact_four_frame_movie(void)
{
    /* frame_count == SLOT_COUNT: prefill exactly fills every slot with
     * no frames left to queue afterwards -- the boundary case for
     * refill_empty_slots' "nothing left to queue" path. */
    uint32_t sectors;
    uint8_t *drive = build_synthetic_drive(4, &sectors);
    global_t global;
    cin2_header_t header;
    bool ok;

    memset(&global, 0, sizeof(global));
    global.usb = (usb_device_t)(uintptr_t)1;
    sim_set_drive(drive, sectors);
    CHECK(cin2_parse_header(drive, &header), "4-frame header parses");

    g_frames_rendered = 0;
    g_async_reads = 0;
    ok = player_v2_run(&global, &header, 0);

    CHECK(ok, "exact 4-frame movie plays to completion");
    CHECK(g_frames_rendered == 4, "exactly 4 frames rendered");
    CHECK(g_async_reads == 4, "exactly 4 reads total, none beyond frame_count");

    free(drive);
}

static void test_empty_movie_rejected(void)
{
    cin2_header_t header;
    global_t global;
    bool ok;

    memset(&header, 0, sizeof(header));
    header.width = CINEMA_V2_WIDTH;
    header.height = CINEMA_V2_HEIGHT;
    header.fps_num = 24;
    header.fps_den = 1;
    header.frame_count = 0;

    memset(&global, 0, sizeof(global));
    global.usb = (usb_device_t)(uintptr_t)1;

    ok = player_v2_run(&global, &header, 0);
    CHECK(!ok, "zero-frame movie is rejected rather than looping forever");
}

int main(void)
{
    test_full_playback_no_resume();
    test_resume_starts_mid_movie();
    test_read_error_is_fatal_and_reported();
    test_single_frame_movie();
    test_exact_four_frame_movie();
    test_empty_movie_rejected();

    if (g_failures == 0) {
        printf("All player_v2 simulation tests passed.\n");
        return 0;
    }

    printf("%d test(s) failed.\n", g_failures);
    return 1;
}
