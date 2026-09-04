#include "cin2.h"
#include <string.h>

static uint16_t read_u16le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32le(const uint8_t *p)
{
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static void write_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void write_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

uint32_t cin2_crc32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;

    for (i = 0; i < length; ++i) {
        uint8_t bit;

        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

bool cin2_frame_count_fits_drive(uint32_t frame_count, uint32_t drive_sectors)
{
    uint64_t required_sectors = (uint64_t)CIN2_DATA_LBA
        + (uint64_t)frame_count * (uint64_t)CIN2_FRAME_SECTORS;

    return required_sectors <= (uint64_t)drive_sectors;
}

bool cin2_has_magic(const uint8_t *raw)
{
    return memcmp(raw, CIN2_MAGIC, 4) == 0;
}

bool cin2_parse_header(const uint8_t *raw, cin2_header_t *out)
{
    uint32_t stored_crc;
    uint32_t computed_crc;
    uint8_t i;

    if (!cin2_has_magic(raw)) {
        return false;
    }
    if (raw[4] != CIN2_VERSION) {
        return false;
    }

    stored_crc = read_u32le(raw + 22);
    computed_crc = cin2_crc32(raw, CIN2_CRC_BYTES);
    if (stored_crc != computed_crc) {
        return false;
    }

    out->width = read_u16le(raw + 6);
    out->height = read_u16le(raw + 8);
    out->fps_num = read_u32le(raw + 10);
    out->fps_den = read_u32le(raw + 14);
    out->frame_count = read_u32le(raw + 18);

    if (out->fps_num == 0 || out->fps_den == 0) {
        return false;
    }

    for (i = 0; i < 16; ++i) {
        out->palette[i] = read_u16le(raw + 26 + i * 2);
    }

    return true;
}

void cin2_build_header(uint8_t *raw, const cin2_header_t *header)
{
    uint8_t i;

    memset(raw, 0, CIN2_HEADER_BYTES);
    memcpy(raw, CIN2_MAGIC, 4);
    raw[4] = CIN2_VERSION;
    raw[5] = 0; /* flags, reserved */
    write_u16le(raw + 6, header->width);
    write_u16le(raw + 8, header->height);
    write_u32le(raw + 10, header->fps_num);
    write_u32le(raw + 14, header->fps_den);
    write_u32le(raw + 18, header->frame_count);
    write_u32le(raw + 22, cin2_crc32(raw, CIN2_CRC_BYTES));

    for (i = 0; i < 16; ++i) {
        write_u16le(raw + 26 + i * 2, header->palette[i]);
    }
}

void cin2_build_resume_record(uint8_t *raw, const cin2_resume_t *state)
{
    /* Bounded scan rather than strlen(): state->filename is a fixed
     * CIN2_RESUME_FILENAME_LEN-byte array, and nothing guarantees it is
     * NUL-terminated within that span, so an unbounded strlen() could
     * read past it. */
    size_t name_len = 0;

    while (name_len < CIN2_RESUME_FILENAME_LEN && state->filename[name_len] != '\0') {
        ++name_len;
    }
    if (name_len >= CIN2_RESUME_FILENAME_LEN) {
        name_len = CIN2_RESUME_FILENAME_LEN - 1;
    }

    memset(raw, 0, CIN2_RESUME_BYTES);
    memcpy(raw, "CR2S", 4);
    raw[4] = CIN2_VERSION;
    write_u32le(raw + 8, state->frame_count);
    write_u32le(raw + 12, state->last_presented_frame);
    memcpy(raw + 16, state->filename, name_len); /* remainder already zeroed above */
    write_u32le(raw + 29, cin2_crc32(raw, 29));
}

bool cin2_parse_resume_record(const uint8_t *raw, cin2_resume_t *out)
{
    uint32_t stored_crc;
    uint32_t computed_crc;

    if (memcmp(raw, "CR2S", 4) != 0) {
        return false;
    }
    if (raw[4] != CIN2_VERSION) {
        return false;
    }

    stored_crc = read_u32le(raw + 29);
    computed_crc = cin2_crc32(raw, 29);
    if (stored_crc != computed_crc) {
        return false;
    }

    out->frame_count = read_u32le(raw + 8);
    out->last_presented_frame = read_u32le(raw + 12);
    memcpy(out->filename, raw + 16, CIN2_RESUME_FILENAME_LEN - 1);
    out->filename[CIN2_RESUME_FILENAME_LEN - 1] = '\0';

    return true;
}
