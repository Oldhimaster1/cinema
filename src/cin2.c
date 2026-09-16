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


static bool add_u32_no_overflow(uint32_t a, uint32_t b, uint32_t *out)
{
    if (b > UINT32_MAX - a) return false;
    *out = a + b;
    return true;
}

bool cin2_parse_placement(const uint8_t *raw, cin2_placement_t *out)
{
    const uint8_t *p = raw + CIN2_PLACEMENT_OFFSET;
    uint16_t extent_count;
    uint16_t descriptor_bytes;
    uint32_t stored_extent_crc;
    uint32_t stored_metadata_crc;
    uint16_t i;

    if (memcmp(p, CIN2_PLACEMENT_MAGIC, 4) != 0
        || p[4] != CIN2_PLACEMENT_VERSION
        || (p[5] & ~CIN2_PLACEMENT_KNOWN_FLAGS) != 0
        || (p[5] & CIN2_PLACEMENT_FLAG_PREPARED) == 0) return false;
    extent_count = read_u16le(p + 6);
    descriptor_bytes = read_u16le(p + 8);
    if (extent_count == 0 || extent_count > CIN2_PLACEMENT_MAX_EXTENTS
        || descriptor_bytes != (uint16_t)(64u + extent_count * 8u)
        || CIN2_PLACEMENT_OFFSET + descriptor_bytes > CIN2_HEADER_BYTES
        || read_u16le(p + 10) != 512u
        || p[12] == 0) return false;

    stored_extent_crc = read_u32le(p + 56);
    stored_metadata_crc = read_u32le(p + 60);
    if (stored_metadata_crc != cin2_crc32(p, 60)
        || stored_extent_crc != cin2_crc32(raw + CIN2_PLACEMENT_EXTENTS_OFFSET,
                                           (uint32_t)extent_count * 8u)) return false;

    memset(out, 0, sizeof(*out));
    out->flags = p[5];
    out->extent_count = extent_count;
    out->descriptor_bytes = descriptor_bytes;
    out->logical_sector_bytes = read_u16le(p + 10);
    out->sectors_per_cluster = p[12];
    out->volume_serial = read_u32le(p + 16);
    out->partition_base_lba = read_u32le(p + 20);
    out->first_fat_lba = read_u32le(p + 24);
    out->first_data_lba = read_u32le(p + 28);
    out->fat_size_sectors = read_u32le(p + 32);
    out->total_data_clusters = read_u32le(p + 36);
    out->movie_first_cluster = read_u32le(p + 40);
    out->movie_file_size = read_u32le(p + 44);
    out->total_movie_sectors = read_u32le(p + 48);
    out->immutable_header_crc = read_u32le(p + 52);
    for (i = 0; i < extent_count; ++i) {
        const uint8_t *e = raw + CIN2_PLACEMENT_EXTENTS_OFFSET + i * 8u;
        out->extents[i].start_lba = read_u32le(e);
        out->extents[i].sector_count = read_u32le(e + 4);
    }
    return true;
}

bool cin2_validate_placement(const cin2_placement_t *m,
                              const cin2_placement_context_t *c)
{
    uint64_t data_end;
    uint64_t sum = 0;
    uint32_t expected_first_lba;
    uint32_t cluster_offset;
    uint16_t i, j;

    if (m == NULL || c == NULL || m->extent_count == 0
        || m->extent_count > CIN2_PLACEMENT_MAX_EXTENTS
        || m->logical_sector_bytes != c->logical_sector_bytes
        || m->sectors_per_cluster != c->sectors_per_cluster
        || m->volume_serial != c->volume_serial
        || m->partition_base_lba != c->partition_base_lba
        || m->first_fat_lba != c->first_fat_lba
        || m->first_data_lba != c->first_data_lba
        || m->fat_size_sectors != c->fat_size_sectors
        || m->total_data_clusters != c->total_data_clusters
        || m->movie_first_cluster != c->movie_first_cluster
        || m->movie_file_size != c->movie_file_size
        || m->immutable_header_crc != c->immutable_header_crc
        || c->logical_sector_bytes != 512u || c->sectors_per_cluster == 0
        || c->movie_first_cluster < 2u) return false;

    if (c->movie_file_size > UINT32_MAX - 511u) return false;
    if (m->total_movie_sectors != (c->movie_file_size + 511u) / 512u) return false;
    data_end = (uint64_t)c->first_data_lba
        + (uint64_t)c->total_data_clusters * c->sectors_per_cluster;
    cluster_offset = c->movie_first_cluster - 2u;
    if (cluster_offset > UINT32_MAX / c->sectors_per_cluster
        || !add_u32_no_overflow(c->first_data_lba,
             cluster_offset * c->sectors_per_cluster, &expected_first_lba)
        || m->extents[0].start_lba != expected_first_lba) return false;

    for (i = 0; i < m->extent_count; ++i) {
        uint64_t start = m->extents[i].start_lba;
        uint64_t end = start + m->extents[i].sector_count;
        if (m->extents[i].sector_count == 0 || start < c->first_data_lba
            || end < start || end > data_end) return false;
        sum += m->extents[i].sector_count;
        if (sum > UINT32_MAX) return false;
        for (j = 0; j < i; ++j) {
            uint64_t other_start = m->extents[j].start_lba;
            uint64_t other_end = other_start + m->extents[j].sector_count;
            if (start < other_end && other_start < end) return false;
        }
    }
    return sum == m->total_movie_sectors;
}

bool cin2_build_placement(uint8_t *raw, const cin2_placement_t *m)
{
    uint8_t *p = raw + CIN2_PLACEMENT_OFFSET;
    uint16_t descriptor_bytes;
    uint16_t i;
    if (raw == NULL || m == NULL || m->extent_count == 0
        || m->extent_count > CIN2_PLACEMENT_MAX_EXTENTS
        || m->logical_sector_bytes != 512u || m->sectors_per_cluster == 0) return false;
    descriptor_bytes = (uint16_t)(64u + m->extent_count * 8u);
    memset(p, 0, CIN2_HEADER_BYTES - CIN2_PLACEMENT_OFFSET);
    memcpy(p, CIN2_PLACEMENT_MAGIC, 4);
    p[4] = CIN2_PLACEMENT_VERSION;
    p[5] = m->flags | CIN2_PLACEMENT_FLAG_PREPARED;
    write_u16le(p + 6, m->extent_count);
    write_u16le(p + 8, descriptor_bytes);
    write_u16le(p + 10, m->logical_sector_bytes);
    p[12] = m->sectors_per_cluster;
    write_u32le(p + 16, m->volume_serial);
    write_u32le(p + 20, m->partition_base_lba);
    write_u32le(p + 24, m->first_fat_lba);
    write_u32le(p + 28, m->first_data_lba);
    write_u32le(p + 32, m->fat_size_sectors);
    write_u32le(p + 36, m->total_data_clusters);
    write_u32le(p + 40, m->movie_first_cluster);
    write_u32le(p + 44, m->movie_file_size);
    write_u32le(p + 48, m->total_movie_sectors);
    write_u32le(p + 52, m->immutable_header_crc);
    for (i = 0; i < m->extent_count; ++i) {
        uint8_t *e = raw + CIN2_PLACEMENT_EXTENTS_OFFSET + i * 8u;
        write_u32le(e, m->extents[i].start_lba);
        write_u32le(e + 4, m->extents[i].sector_count);
    }
    write_u32le(p + 56, cin2_crc32(raw + CIN2_PLACEMENT_EXTENTS_OFFSET,
                                    (uint32_t)m->extent_count * 8u));
    write_u32le(p + 60, cin2_crc32(p, 60));
    return true;
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

    out->flags = raw[5];
    if (out->flags & (uint8_t)~CIN2_FLAG_PACKED4) return false;
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
    raw[5] = header->flags;
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

int cin2_resume_store_find(const uint8_t *raw, const char *filename, cin2_resume_t *out)
{
    int i;

    for (i = 0; i < CIN2_RESUME_SLOT_COUNT; ++i) {
        cin2_resume_t candidate;

        if (cin2_parse_resume_record(raw + (size_t)i * CIN2_RESUME_BYTES, &candidate)
            && strcmp(candidate.filename, filename) == 0) {
            *out = candidate;
            return i;
        }
    }

    return -1;
}

int cin2_resume_store_slot_for(const uint8_t *raw, const char *filename)
{
    int first_invalid = -1;
    int i;

    for (i = 0; i < CIN2_RESUME_SLOT_COUNT; ++i) {
        cin2_resume_t candidate;
        bool valid = cin2_parse_resume_record(raw + (size_t)i * CIN2_RESUME_BYTES, &candidate);

        if (valid && strcmp(candidate.filename, filename) == 0) {
            return i;
        }
        if (!valid && first_invalid < 0) {
            first_invalid = i;
        }
    }

    return first_invalid >= 0 ? first_invalid : 0;
}

void cin2_resume_store_write_slot(uint8_t *raw, int slot, const cin2_resume_t *state)
{
    cin2_build_resume_record(raw + (size_t)slot * CIN2_RESUME_BYTES, state);
}
