/* Host-side unit tests for src/cin2.c.
 *
 * This file is deliberately free of calculator-specific headers so it
 * can be compiled and tested with a normal host compiler -- there is no
 * CE toolchain available in this environment to build/run the real
 * .8xp. Build and run with tests/run_tests.sh.
 *
 * There used to be pixel-unpack tests here too (this file was named
 * test_decode.c), when src/decode.c bit-packed frames 2-per-byte and
 * needed a decode step. Frames are now raw, unpacked bytes read straight
 * off USB into the sprite buffer (see src/player_v2.c) -- there's no
 * decode step left to test.
 */
#include "../src/cin2.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_failures++; \
        } \
    } while (0)

/* --- cin2 header tests ----------------------------------------------- */

static void test_header_round_trip(void)
{
    uint8_t raw[CIN2_HEADER_BYTES];
    cin2_header_t in, out;
    int i;

    memset(&in, 0, sizeof(in));
    in.width = 160;
    in.height = 96;
    in.fps_num = 24000;
    in.fps_den = 1001;
    in.frame_count = 123456;
    for (i = 0; i < 16; ++i) {
        in.palette[i] = (uint16_t)(i * 0x1111);
    }

    cin2_build_header(raw, &in);

    CHECK(cin2_has_magic(raw), "magic present after build");
    CHECK(cin2_parse_header(raw, &out), "parse succeeds on freshly built header");
    CHECK(out.width == in.width, "width round-trips");
    CHECK(out.height == in.height, "height round-trips");
    CHECK(out.fps_num == in.fps_num, "fps_num round-trips");
    CHECK(out.fps_den == in.fps_den, "fps_den round-trips");
    CHECK(out.frame_count == in.frame_count, "frame_count round-trips");
    for (i = 0; i < 16; ++i) {
        CHECK(out.palette[i] == in.palette[i], "palette entry round-trips");
    }

    /* Corrupting any byte in the CRC-covered range must be detected. */
    for (i = 0; i < CIN2_CRC_BYTES; ++i) {
        uint8_t corrupt[CIN2_HEADER_BYTES];
        memcpy(corrupt, raw, sizeof(corrupt));
        corrupt[i] ^= 0xFF;
        CHECK(!cin2_parse_header(corrupt, &out), "corrupted header byte rejected");
    }

    /* Wrong magic must be rejected outright (this is how v1-vs-v2 drive
     * detection relies on this function not false-accepting). */
    {
        uint8_t bad_magic[CIN2_HEADER_BYTES];
        memcpy(bad_magic, raw, sizeof(bad_magic));
        memcpy(bad_magic, "CIN1", 4);
        /* Byte 4 corruption already covered above; here we specifically
         * check the magic-check short-circuit fires without even
         * reaching the CRC check. */
        CHECK(!cin2_has_magic(bad_magic), "modified magic not recognized");
    }

    /* fps_num == 0 or fps_den == 0 must be rejected even with a valid
     * CRC, since it would divide-by-zero in the player's scheduler. */
    {
        cin2_header_t zero_fps = in;
        uint8_t zraw[CIN2_HEADER_BYTES];
        zero_fps.fps_num = 0;
        cin2_build_header(zraw, &zero_fps);
        CHECK(!cin2_parse_header(zraw, &out), "fps_num == 0 rejected");
    }
}

static void test_v1_drive_not_detected_as_v2(void)
{
    /* A v1 drive's LBA 0 is a raw 256-entry RGB565 palette (512 bytes of
     * essentially arbitrary color data), not a CIN2 header. Simulate a
     * plausible v1 palette sector and confirm it doesn't get misdetected
     * as v2 magic "CIN2" (0x43 0x49 0x4E 0x32). */
    uint8_t raw[CIN2_HEADER_BYTES];
    unsigned i;

    for (i = 0; i < sizeof(raw); ++i) {
        raw[i] = (uint8_t)((i * 37 + 11) & 0xFF);
    }
    CHECK(!cin2_has_magic(raw), "pseudo-random v1 palette sector doesn't match CIN2 magic");
}

/* --- resume record tests ---------------------------------------------- */

static void test_resume_round_trip(void)
{
    uint8_t raw[CIN2_RESUME_BYTES];
    cin2_resume_t in, out;
    int i;

    in.frame_count = 7200;
    in.last_presented_frame = 3141;
    strcpy(in.filename, "MOVIE01.BIN");

    cin2_build_resume_record(raw, &in);
    CHECK(cin2_parse_resume_record(raw, &out), "resume record parses");
    CHECK(out.frame_count == in.frame_count, "resume frame_count round-trips");
    CHECK(out.last_presented_frame == in.last_presented_frame,
          "resume last_presented_frame round-trips");
    CHECK(strcmp(out.filename, in.filename) == 0, "resume filename round-trips");

    {
        /* Empty filename (raw single-image mode) round-trips too. */
        cin2_resume_t raw_mode_in, raw_mode_out;

        raw_mode_in.frame_count = 100;
        raw_mode_in.last_presented_frame = 50;
        raw_mode_in.filename[0] = '\0';

        cin2_build_resume_record(raw, &raw_mode_in);
        CHECK(cin2_parse_resume_record(raw, &raw_mode_out), "raw-mode (empty filename) record parses");
        CHECK(raw_mode_out.filename[0] == '\0', "empty filename round-trips as empty");
    }

    for (i = 0; i < CIN2_RESUME_BYTES; ++i) {
        uint8_t corrupt[CIN2_RESUME_BYTES];
        memcpy(corrupt, raw, sizeof(corrupt));
        corrupt[i] ^= 0xFF;
        CHECK(!cin2_parse_resume_record(corrupt, &out), "corrupted resume record rejected");
    }

    {
        /* A v1 resume appvar is just a 4-byte LBA -- far shorter than 20
         * bytes and with no reason to start with "CR2S". Confirm garbage
         * that happens to be the right length is still rejected. */
        uint8_t garbage[CIN2_RESUME_BYTES] = {0};
        CHECK(!cin2_parse_resume_record(garbage, &out), "all-zero buffer rejected");
    }
}

/* --- crc32 known-answer test ------------------------------------------ */

static void test_crc32_known_vectors(void)
{
    /* CRC-32 of the empty string is 0, and of "123456789" is the
     * well-known 0xCBF43926 check value for this polynomial/variant. */
    CHECK(cin2_crc32((const uint8_t *)"", 0) == 0x00000000u, "crc32 of empty input");
    CHECK(cin2_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u,
          "crc32 check value for \"123456789\"");
}

static void test_frame_count_fits_drive(void)
{
    /* header sector + frame_count*CIN2_FRAME_SECTORS must be <= drive_sectors */
    CHECK(cin2_frame_count_fits_drive(0, 1), "zero frames always fits (just the header)");
    CHECK(cin2_frame_count_fits_drive(0, 0) == false, "not even the header sector fits an empty drive");
    CHECK(cin2_frame_count_fits_drive(10, 1 + 10 * CIN2_FRAME_SECTORS), "exact fit is accepted");
    CHECK(cin2_frame_count_fits_drive(10, 1 + 10 * CIN2_FRAME_SECTORS - 1) == false, "one sector short is rejected");
    CHECK(cin2_frame_count_fits_drive(10, 1 + 10 * CIN2_FRAME_SECTORS + 1), "drive larger than needed is accepted");
    /* A hostile/corrupt frame_count near UINT32_MAX must not wrap 32-bit
     * math into looking small -- this is exactly the case a 64-bit
     * intermediate is required for. */
    CHECK(cin2_frame_count_fits_drive(0xFFFFFFFFu, 1000) == false,
          "huge frame_count against a small drive is rejected, not wrapped");
}

int main(void)
{
    test_header_round_trip();
    test_v1_drive_not_detected_as_v2();
    test_resume_round_trip();
    test_crc32_known_vectors();
    test_frame_count_fits_drive();

    if (g_failures == 0) {
        printf("All decode/cin2 tests passed.\n");
        return 0;
    }

    printf("%d test(s) failed.\n", g_failures);
    return 1;
}
