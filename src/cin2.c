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

bool cin2_has_magic(const uint8_t *raw)
{
    return memcmp(raw, CIN2_MAGIC, 4) == 0;
}

bool cin2_parse_header(const uint8_t *raw, cin2_header_t *out)
{
    uint32_t stored_crc;
    uint32_t computed_crc;
    uint16_t i;

    if (!cin2_has_magic(raw)) {
        return false;
    }
    if (raw[4] != CIN2_VERSION) {
        return false;
    }
    if (raw[5] != 0) {
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
    out->title[0] = 0;
    out->chapter_count = 0;
    if (memcmp(raw + 58, "C2MD", 4) == 0) {
        uint8_t title_len = raw[63];
        uint8_t count = raw[64];
        uint32_t stored_meta_crc = read_u32le(raw + 402);
        if (raw[62] != 1 || raw[65] != 0 || title_len > CIN2_TITLE_MAX
            || count > CIN2_CHAPTER_MAX
            || stored_meta_crc != cin2_crc32(raw + 58, 344)) return false;
        memcpy(out->title, raw + 66, title_len);
        out->title[title_len] = 0;
        out->chapter_count = count;
        for (i = 0; i < count; ++i) {
            uint16_t off = (uint16_t)(114 + i * 24);
            uint8_t len = raw[off + 4];
            if (len > CIN2_CHAPTER_NAME_MAX) return false;
            out->chapters[i].frame = read_u32le(raw + off);
            if (out->chapters[i].frame >= out->frame_count) return false;
            memcpy(out->chapters[i].name, raw + off + 5, len);
            out->chapters[i].name[len] = 0;
        }
        for (i = 406; i < CIN2_HEADER_BYTES; ++i) if (raw[i] != 0) return false;
    } else {
        for (i = 58; i < CIN2_HEADER_BYTES; ++i) if (raw[i] != 0) return false;
    }
    return true;
}

bool cin2_stream_fits(uint32_t frame_count, uint32_t drive_blocks)
{
    if (frame_count == 0 || drive_blocks <= CIN2_DATA_LBA) {
        return false;
    }
    return frame_count <=
        (drive_blocks - CIN2_DATA_LBA) / CIN2_FRAME_SECTORS;
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

    for (i = 0; i < 16; ++i) write_u16le(raw + 26 + i * 2, header->palette[i]);
    if (header->title[0] || header->chapter_count) {
        uint8_t title_len = (uint8_t)strlen(header->title);
        if (title_len > CIN2_TITLE_MAX) title_len = CIN2_TITLE_MAX;
        memcpy(raw + 58, "C2MD", 4); raw[62] = 1; raw[63] = title_len;
        raw[64] = header->chapter_count <= CIN2_CHAPTER_MAX ? header->chapter_count : CIN2_CHAPTER_MAX;
        memcpy(raw + 66, header->title, title_len);
        for (i = 0; i < raw[64]; ++i) {
            uint16_t off = (uint16_t)(114 + i * 24);
            uint8_t len = (uint8_t)strlen(header->chapters[i].name);
            if (len > CIN2_CHAPTER_NAME_MAX) len = CIN2_CHAPTER_NAME_MAX;
            write_u32le(raw + off, header->chapters[i].frame); raw[off + 4] = len;
            memcpy(raw + off + 5, header->chapters[i].name, len);
        }
        write_u32le(raw + 402, cin2_crc32(raw + 58, 344));
    }
}

void cin2_build_resume_record(uint8_t *raw, const cin2_resume_t *state)
{
    memset(raw, 0, CIN2_RESUME_BYTES);
    memcpy(raw, "CR2S", 4);
    raw[4] = CIN2_VERSION;
    write_u32le(raw + 8, state->frame_count);
    write_u32le(raw + 12, state->last_presented_frame);
    write_u32le(raw + 16, cin2_crc32(raw, 16));
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
    if (raw[5] != 0) {
        return false;
    }

    stored_crc = read_u32le(raw + 16);
    computed_crc = cin2_crc32(raw, 16);
    if (stored_crc != computed_crc) {
        return false;
    }

    out->frame_count = read_u32le(raw + 8);
    out->last_presented_frame = read_u32le(raw + 12);

    return true;
}
