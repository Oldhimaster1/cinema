# Cinema v2 (CIN2) on-disk format

This document is the single source of truth for the binary layout used by
both the calculator player (`src/cin2.c`, `src/player_v2.c`) and the host
encoder (`tools/encode_cin2.py`). If you change one, change the other and
update this file in the same commit.

All multi-byte integers are **little-endian**. All sizes are in bytes
unless stated otherwise. The target media uses 512-byte logical sectors
(LBA = logical block address, one sector each).

## Why a new format

The original Cinema format stores, per frame, a full 256-color palette
(1 sector) followed by an uncompressed 160x96 8bpp image (30 sectors) --
31 sectors/frame, 15,872 bytes/frame. At 24 fps that is ~372 KiB/s, above
what the CE Toolchain documents for tested USB mass-storage throughput
(~262-273 KiB/s). It also requires two transfers (and two callbacks) per
frame and re-installs the palette every frame.

CIN2 uses one shared 16-color palette for the whole movie (stored once in
the header, installed once at startup) instead of resending a palette
every frame. Two frame-storage modes are supported:

```
160 * 96 = 15,360 bytes = 30 sectors
```

In RAW8 mode (flags 0), a frame is one byte per pixel (0..15), 30 sectors.
At 24fps this requires ~360 KiB/s, above the ~262-273 KiB/s documented
throughput of tested USB drives. At 15fps the requirement is ~225 KiB/s.

In Packed4 mode (flags bit 0 set), a frame uses two 4-bit palette indices
per byte: 7,680 bytes or 15 sectors. The **first** pixel is stored in the
high nibble, the second in the low nibble. Packed4 needs only ~180 KiB/s
at 24fps and ~225 KiB/s at 30fps, before command overhead. The
specialized `fixed_asm` eZ80 renderer reads Packed4 directly; neither
the default GraphX renderer nor fixed-C supports Packed4. Unsupported
renderer builds reject Packed4 rather than showing incorrect pixels.

An **earlier** Packed4 experiment unpacked the pixels in software before
passing them to GraphX. Physical testing found the unpacking cost
outweighed the I/O savings. The newer direct-drawing assembly renderer
removes that intermediate unpacking step. Packed4 playback was tested on
the specific Beta 1 calculator setup; its FPS still depends on the drive
and renderer, and cannot be inferred from transfer rates alone.

## Drive layout

```
LBA 0:            CIN2 header (512 bytes, one sector)
LBA 1..S:         frame 0
LBA 1+S..2*S:     frame 1
LBA 1+2*S..3*S:   frame 2
...
LBA 1+S*n .. +S-1: frame n
```

`S` is 30 sectors for RAW8 or 15 for Packed4. There is no per-frame header,
palette, or padding -- frames are back-to-back.

## Header (LBA 0, 512 bytes)

| Offset | Size | Field          | Notes                                              |
|-------:|-----:|----------------|-----------------------------------------------------|
| 0      | 4    | `magic`        | ASCII `"CIN2"` (0x43 0x49 0x4E 0x32)                |
| 4      | 1    | `version`      | `2`                                                  |
| 5      | 1    | `flags`        | bit 0: Packed4; all other bits reserved/must be zero |
| 6      | 2    | `width`        | u16, logical pixel width. Always `160` in v2.0       |
| 8      | 2    | `height`       | u16, logical pixel height. Always `96` in v2.0       |
| 10     | 4    | `fps_num`      | u32, frame rate numerator (e.g. `24` or `24000`)     |
| 14     | 4    | `fps_den`      | u32, frame rate denominator (e.g. `1` or `1001`)     |
| 18     | 4    | `frame_count`  | u32, total frames in the stream                      |
| 22     | 4    | `header_crc32` | u32, CRC-32 (IEEE 802.3 poly) over bytes `[0, 22)`    |
| 26     | 32   | `palette`      | 16 entries x 2 bytes each, RGB1555, index 0..15       |
| 58     | 454  | reserved       | must be zero-filled                                   |

`palette` entries are written verbatim into the LCD's hardware palette
(`gfx_SetPalette(header.palette, 32, 0)`), packed as RGB1555 -- the same
5-5-5-with-an-unused-top-bit layout as GraphX's own `gfx_RGBTo1555`
macro (`((r>>3)<<10) | ((g>>3)<<5) | (b>>3)`). An earlier version of
this doc incorrectly claimed `gfx_SetPalette` wanted plain 5-6-5
instead -- an assumption that was never actually checked against the
real `graphx.h`, which has no RGB565 macro or format at all, only
1555. Real-hardware testing surfaced the mistake as visibly wrong
colors; `tools/encode_cin2.py`'s `rgb888_to_rgb1555` and this format
now agree with the real header.

Only the pre-CRC fields (`magic` through `frame_count`, 22 bytes) are
covered by `header_crc32`. The palette and pixel data are not checksummed
-- corruption there just looks wrong, it doesn't desync playback, and
checksumming hundreds of KiB/s of pixel data on an ez80 core is not worth
the CPU budget. A reader that gets a bad header CRC must refuse to play
rather than guess at corrupted `fps_num`/`frame_count`/`width`/`height`
values, since those drive LBA arithmetic.

## Frame payload

RAW8 frames use 15,360 bytes, one palette index per byte in row-major
order, which GraphX can draw directly. Packed4 frames use 7,680 bytes:
each byte stores consecutive pixels `(p0 << 4) | p1`, high nibble first.
The direct assembly renderer draws these without a separate unpack buffer.
There is no row padding in either mode.

## Resume record (TI AppVar `SSCINEV2`)

The appvar holds `CIN2_RESUME_SLOT_COUNT` (8) of these records
back-to-back (264 bytes total), so up to 8 different movies on a drive
each keep their own resume position instead of only the most recently
watched one sharing a single slot. Each slot is independently
CRC-validated; an empty or corrupted slot just reads as "no resume
here". Written (read-modify-write over the whole store) on exit, read
on the "resume?" prompt. Each record is 33 bytes, all little-endian:

| Offset | Size | Field                  |
|-------:|-----:|-------------------------|
| 0      | 4    | `magic` = `"CR2S"`      |
| 4      | 1    | `version` = `2`         |
| 5      | 3    | reserved (zero)         |
| 8      | 4    | `frame_count`           |
| 12     | 4    | `last_presented_frame`  |
| 16     | 13   | `filename` (NUL-terminated short name, `""` for raw whole-device-image mode) |
| 29     | 4    | `record_crc32` (over bytes `[0, 29)`) |

`frame_count` is stored so a resume record from a *different* movie (or a
re-encoded one with a different length) is detected and discarded rather
than silently seeking to a frame number that may not exist, or that
exists but belongs to different content. `last_presented_frame` -- not
the read-ahead/queue position -- is what gets saved, since the read-ahead
queue can be several frames ahead of what was actually shown.

`filename` exists because a FAT32 drive can hold more than one movie:
`frame_count` alone isn't a reliable "same movie" check, since two
different files could coincidentally have the same frame count. The
player compares both the stored `filename` and `frame_count` against the
file currently being opened before offering to resume into it. It holds
the file's short (8.3) name as reported by the FAT32 directory listing,
truncated to 12 characters plus a NUL if longer. Raw whole-device-image
playback (no FAT32 filesystem, movie image written directly to the drive)
has no filename, so it stores `""` and only `frame_count` is checked in
that mode, matching the original v2 behavior.

This is a distinct AppVar from the original `SSCINEMA` (which stores a
raw 4-byte LBA for the v1 format) so v1 and v2 resume state never collide
and a v1 drive's resume prompt is unaffected by v2 usage or vice versa.

## CRC-32

Standard CRC-32 (IEEE 802.3), polynomial `0xEDB88320`, init `0xFFFFFFFF`,
final XOR `0xFFFFFFFF` -- the same variant used by zlib/gzip/PNG. Both
`tools/cin2_format.py` and `src/cin2.c` implement it independently from
this spec; `tests/test_cin2_format.py` checks them against known test
vectors.


## Optional embedded placement extension (CPE1)

CIN2 sector zero reserves bytes 64 through 383 for an optional destination-specific placement descriptor. The extension does not alter the ordinary CIN2 header CRC or move frame data from sector 1. Unprepared files leave the region zero-filled. Prepared files use magic `CPE1`, version 1, a prepared flag, FAT32 volume and movie identity fields, separate metadata and extent CRC-32 values, and up to 32 `{start_lba, sector_count}` entries. A player must reject the extension unless every identity, range, first-cluster, non-overlap, and sector-sum check succeeds. Rejection falls back to ordinary FAT mapping.
