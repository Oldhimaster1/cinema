/* Host-side unit tests for src/decode.c and src/cin2.c.
 *
 * These two files are deliberately free of calculator-specific headers
 * so they can be compiled and tested with a normal host compiler --
 * there is no CE toolchain available in this environment to build/run
 * the real .8xp. Build and run with tests/run_tests.sh.
 */
#include "../src/decode.h"
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

/* --- decode tests --------------------------------------------------- */

static void test_decode_known_pattern(void)
{
    uint8_t packed[CINEMA_V2_PACKED_BYTES];
    /* 320-wide framebuffer, 240 rows -- same shape as gfx_vbuffer. */
    static uint8_t fb[240 * 320];
    uint16_t stride = 320;
    uint16_t y_offset = 24;
    uint16_t x, y;

    memset(packed, 0, sizeof(packed));
    memset(fb, 0xAA, sizeof(fb)); /* sentinel so untouched rows are obvious */

    /* First byte of frame: left nibble = 0x3, right nibble = 0xC. */
    packed[0] = 0x3C;
    /* Last byte of frame (bottom-right pair): left=0x1, right=0x2. */
    packed[CINEMA_V2_PACKED_BYTES - 1] = 0x12;

    cinema_draw_packed4_scaled2x(packed, fb, stride, y_offset);

    /* Row 0 destination rows are y_offset+0 and y_offset+1, columns 0..3
     * should be {3,3,12,12}. */
    CHECK(fb[(y_offset + 0) * stride + 0] == 3, "top-left px0 row0");
    CHECK(fb[(y_offset + 0) * stride + 1] == 3, "top-left px1 row0");
    CHECK(fb[(y_offset + 0) * stride + 2] == 12, "top-left px2 row0");
    CHECK(fb[(y_offset + 0) * stride + 3] == 12, "top-left px3 row0");
    CHECK(fb[(y_offset + 1) * stride + 0] == 3, "top-left px0 row1 (2x vert)");
    CHECK(fb[(y_offset + 1) * stride + 3] == 12, "top-left px3 row1 (2x vert)");

    /* Bottom-right 2x2 block: destination rows
     * y_offset + (HEIGHT-1)*2 and +1, columns DEST_WIDTH-4..DEST_WIDTH-1. */
    {
        uint16_t by0 = y_offset + (CINEMA_V2_HEIGHT - 1) * 2;
        uint16_t by1 = by0 + 1;
        uint16_t bx = CINEMA_V2_DEST_WIDTH - 4;

        CHECK(fb[by0 * stride + bx + 0] == 1, "bottom-right px0");
        CHECK(fb[by0 * stride + bx + 1] == 1, "bottom-right px1");
        CHECK(fb[by0 * stride + bx + 2] == 2, "bottom-right px2");
        CHECK(fb[by0 * stride + bx + 3] == 2, "bottom-right px3");
        CHECK(fb[by1 * stride + bx + 3] == 2, "bottom-right px3 row+1");
    }

    /* Rows above y_offset and below the decoded block must be untouched. */
    CHECK(fb[0] == 0xAA, "row 0 untouched");
    CHECK(fb[(y_offset - 1) * stride] == 0xAA, "row just above block untouched");
    {
        uint16_t after = y_offset + CINEMA_V2_DEST_HEIGHT;
        if (after < 240) {
            CHECK(fb[after * stride] == 0xAA, "row just below block untouched");
        }
    }

    /* Every nibble value 0..15 must decode to itself (index-preserving,
     * no remapping) -- exhaustive over one synthetic frame. */
    for (y = 0; y < CINEMA_V2_HEIGHT; ++y) {
        for (x = 0; x < CINEMA_V2_WIDTH / 2; ++x) {
            uint32_t idx = (uint32_t)y * (CINEMA_V2_WIDTH / 2) + x;
            uint8_t left = (uint8_t)((x + y) % 16);
            uint8_t right = (uint8_t)((x + y + 7) % 16);
            packed[idx] = (uint8_t)((left << 4) | right);
        }
    }
    cinema_draw_packed4_scaled2x(packed, fb, stride, y_offset);
    for (y = 0; y < CINEMA_V2_HEIGHT; ++y) {
        for (x = 0; x < CINEMA_V2_WIDTH / 2; ++x) {
            uint8_t left = (uint8_t)((x + y) % 16);
            uint8_t right = (uint8_t)((x + y + 7) % 16);
            uint16_t dy0 = y_offset + y * 2;
            uint16_t dy1 = dy0 + 1;
            uint16_t dx = x * 4;
            int ok =
                fb[dy0 * stride + dx + 0] == left &&
                fb[dy0 * stride + dx + 1] == left &&
                fb[dy0 * stride + dx + 2] == right &&
                fb[dy0 * stride + dx + 3] == right &&
                fb[dy1 * stride + dx + 0] == left &&
                fb[dy1 * stride + dx + 1] == left &&
                fb[dy1 * stride + dx + 2] == right &&
                fb[dy1 * stride + dx + 3] == right;
            if (!ok) {
                printf("FAIL: exhaustive mismatch at x=%u y=%u\n",
                       (unsigned)x, (unsigned)y);
                g_failures++;
                return;
            }
        }
    }
}

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
    /* header sector + frame_count*15 must be <= drive_sectors */
    CHECK(cin2_frame_count_fits_drive(0, 1), "zero frames always fits (just the header)");
    CHECK(cin2_frame_count_fits_drive(0, 0) == false, "not even the header sector fits an empty drive");
    CHECK(cin2_frame_count_fits_drive(10, 1 + 10 * 15), "exact fit is accepted");
    CHECK(cin2_frame_count_fits_drive(10, 1 + 10 * 15 - 1) == false, "one sector short is rejected");
    CHECK(cin2_frame_count_fits_drive(10, 1 + 10 * 15 + 1), "drive larger than needed is accepted");
    /* A hostile/corrupt frame_count near UINT32_MAX must not wrap 32-bit
     * math into looking small -- this is exactly the case a 64-bit
     * intermediate is required for. */
    CHECK(cin2_frame_count_fits_drive(0xFFFFFFFFu, 1000) == false,
          "huge frame_count against a small drive is rejected, not wrapped");
}

int main(void)
{
    test_decode_known_pattern();
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
