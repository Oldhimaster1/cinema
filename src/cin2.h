#ifndef CINEMA_CIN2_H
#define CINEMA_CIN2_H

/* CIN2 header / resume-record parsing. Pure byte-level logic, no
 * calculator-specific headers, so this compiles and is unit-tested on a
 * host machine too -- see tests/test_decode.c. Binary layout is defined
 * in docs/CIN2_FORMAT.md; keep both in sync. */

#include <stdint.h>
#include <stdbool.h>

#define CIN2_MAGIC "CIN2"
#define CIN2_VERSION       2
#define CIN2_HEADER_BYTES  512
#define CIN2_CRC_BYTES     22  /* bytes [0, 22) covered by header_crc32 */
#define CIN2_DATA_LBA      1
#define CIN2_FRAME_SECTORS 15
#define CIN2_TITLE_MAX 48
#define CIN2_CHAPTER_MAX 12
#define CIN2_CHAPTER_NAME_MAX 19

typedef struct { uint32_t frame; char name[CIN2_CHAPTER_NAME_MAX + 1]; } cin2_chapter_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t frame_count;
    uint16_t palette[16]; /* raw RGB565, ready for gfx_SetPalette */
    char title[CIN2_TITLE_MAX + 1];
    uint8_t chapter_count;
    cin2_chapter_t chapters[CIN2_CHAPTER_MAX];
} cin2_header_t;

/* Cheap check: true if raw[0..3] == "CIN2". Used to distinguish a v2
 * drive from a v1 (headerless) drive before fully validating. */
bool cin2_has_magic(const uint8_t *raw);

/* Parses and validates CIN2_HEADER_BYTES bytes of raw header data
 * (magic, version, CRC). Returns false (leaving *out unspecified) on any
 * mismatch. Does not check width/height against the decoder's supported
 * 160x96 -- callers must do that themselves. */
bool cin2_parse_header(const uint8_t *raw, cin2_header_t *out);

/* Inverse of cin2_parse_header: serializes *header into
 * raw[0..CIN2_HEADER_BYTES), zero-filling reserved bytes and computing
 * the CRC. Not used by the calculator player (which only ever reads
 * headers), but used by tests and available for a future encoder. */
void cin2_build_header(uint8_t *raw, const cin2_header_t *header);

static inline uint32_t cin2_frame_lba(uint32_t frame_number)
{
    return (uint32_t)CIN2_DATA_LBA
        + frame_number * (uint32_t)CIN2_FRAME_SECTORS;
}

/* Standard CRC-32 (IEEE 802.3): poly 0xEDB88320, init/final XOR
 * 0xFFFFFFFF -- the zlib/gzip/PNG variant. */
uint32_t cin2_crc32(const uint8_t *data, uint32_t length);
/* Returns true when every frame lies within a drive of drive_blocks
 * 512-byte sectors, without overflowing the LBA arithmetic. */
bool cin2_stream_fits(uint32_t frame_count, uint32_t drive_blocks);


/* Resume record (AppVar SSCINEV2), see docs/CIN2_FORMAT.md. */
#define CIN2_RESUME_BYTES 20

typedef struct {
    uint32_t frame_count;
    uint32_t last_presented_frame;
} cin2_resume_t;

void cin2_build_resume_record(uint8_t *raw, const cin2_resume_t *state);

/* Returns true and fills *out if raw is a valid v2 resume record. */
bool cin2_parse_resume_record(const uint8_t *raw, cin2_resume_t *out);

#endif
