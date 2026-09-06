#ifndef CINEMA_CIN2_H
#define CINEMA_CIN2_H

/* CIN2 header / resume-record parsing. Pure byte-level logic, no
 * calculator-specific headers, so this compiles and is unit-tested on a
 * host machine too -- see tests/test_cin2.c. Binary layout is defined
 * in docs/CIN2_FORMAT.md; keep both in sync. */

#include <stdint.h>
#include <stdbool.h>

#define CIN2_MAGIC "CIN2"
#define CIN2_VERSION       2
#define CIN2_HEADER_BYTES  512
#define CIN2_CRC_BYTES     22  /* bytes [0, 22) covered by header_crc32 */
#define CIN2_DATA_LBA      1
/* One byte per pixel (160*96 = 15,360 bytes = 30 sectors), not bit-packed:
 * see src/player_v2.c's frame_slot_t comment for why -- real hardware
 * testing traced most of v2's decode-ceiling gap to the CPU cost of
 * unpacking bit-packed pixels on the ez80 core, which outweighed the I/O
 * savings from packing them in the first place. */
#define CIN2_FRAME_SECTORS 30

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t frame_count;
    uint16_t palette[16]; /* raw RGB1555 (gfx_RGBTo1555 layout), ready for gfx_SetPalette */
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

/* True if a frame_count-frame movie's data (header sector plus every
 * frame's 15 sectors) fits entirely within a drive reporting
 * drive_sectors total logical blocks. Uses 64-bit arithmetic so a
 * corrupt/hostile frame_count can't wrap 32-bit LBA math into looking
 * in-bounds. Callers must reject media where this returns false rather
 * than queueing reads that could run past the reported drive capacity. */
bool cin2_frame_count_fits_drive(uint32_t frame_count, uint32_t drive_sectors);

/* Standard CRC-32 (IEEE 802.3): poly 0xEDB88320, init/final XOR
 * 0xFFFFFFFF -- the zlib/gzip/PNG variant. */
uint32_t cin2_crc32(const uint8_t *data, uint32_t length);

/* Resume record (AppVar SSCINEV2), see docs/CIN2_FORMAT.md.
 *
 * filename identifies which movie this record belongs to: with a FAT32
 * drive potentially holding several movies (see src/fat32ro.h), two
 * different files can easily share the same frame_count by coincidence,
 * so frame_count alone is no longer sufficient to tell "resume this
 * movie" from "resume a different one that happens to have the same
 * length". For the raw single-whole-device-image mode (no filesystem,
 * one movie per drive) filename is simply empty on both the saved and
 * compared side, which preserves that mode's original frame_count-only
 * behavior exactly. filename is a NUL-terminated short-name string (see
 * fat32ro_dirent_t.name), truncated to fit if longer. */
#define CIN2_RESUME_BYTES        33
#define CIN2_RESUME_FILENAME_LEN 13

typedef struct {
    uint32_t frame_count;
    uint32_t last_presented_frame;
    char filename[CIN2_RESUME_FILENAME_LEN]; /* NUL-terminated, "" for raw mode */
} cin2_resume_t;

void cin2_build_resume_record(uint8_t *raw, const cin2_resume_t *state);

/* Returns true and fills *out if raw is a valid v2 resume record. */
bool cin2_parse_resume_record(const uint8_t *raw, cin2_resume_t *out);

/* Multi-slot resume store: CIN2_RESUME_SLOT_COUNT independent resume
 * records back-to-back in one CIN2_RESUME_STORE_BYTES appvar, so more
 * than one movie's resume position can be remembered at once (a FAT32
 * drive can hold several -- see docs/CIN2_FORMAT.md). Each slot is
 * validated independently via cin2_parse_resume_record, so a slot
 * that's never been written (all zero) or corrupted just reads as "no
 * resume here" -- no separate "is this slot in use" flag needed. */
#define CIN2_RESUME_SLOT_COUNT  8
#define CIN2_RESUME_STORE_BYTES (CIN2_RESUME_BYTES * CIN2_RESUME_SLOT_COUNT)

/* Searches a CIN2_RESUME_STORE_BYTES-long store for a valid slot whose
 * filename matches. Returns the slot index (0..CIN2_RESUME_SLOT_COUNT-1)
 * and fills *out, or -1 if none matches -- including when raw is an
 * all-zero/freshly-created store, or garbage from an older single-slot
 * build (see cin2_resume_store_slot_for), which correctly find nothing
 * rather than misreading it. Matching is by filename alone; the caller
 * (which has the movie's current header) still must check
 * frame_count/last_presented_frame itself, same as the original
 * single-slot design always did. */
int cin2_resume_store_find(const uint8_t *raw, const char *filename, cin2_resume_t *out);

/* Picks which slot a new resume record for filename should be written
 * into: reuses the slot already holding a record for this exact
 * filename if one exists (so re-watching the same movie updates its own
 * slot instead of spawning a duplicate), else the first slot that fails
 * to parse (empty or corrupt), else slot 0. There's no recency
 * tracking, so once all CIN2_RESUME_SLOT_COUNT slots are genuinely in
 * use by that many different movies, the oldest-index slot is simply
 * reused next -- not true LRU, just a bound on how much resume history
 * is kept. */
int cin2_resume_store_slot_for(const uint8_t *raw, const char *filename);

/* Writes state into slot `slot` (0..CIN2_RESUME_SLOT_COUNT-1) of the
 * CIN2_RESUME_STORE_BYTES-long raw buffer, in place. */
void cin2_resume_store_write_slot(uint8_t *raw, int slot, const cin2_resume_t *state);

#endif
