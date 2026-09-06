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
the header) and packs each pixel into 4 bits. A frame is then exactly:

```
160 * 96 / 2 = 7,680 bytes = 15 sectors
```

At 24 fps: `15 * 512 * 24 = 184,320 bytes/s` (~180 KiB/s) -- comfortably
inside the documented throughput budget, with only one transfer and one
callback per frame and zero per-frame palette installs.

## Drive layout

```
LBA 0:            CIN2 header (512 bytes, one sector)
LBA 1..15:        frame 0, packed 4-bit pixels (7,680 bytes, 15 sectors)
LBA 16..30:       frame 1
LBA 31..45:       frame 2
...
LBA 1+15*n .. +14: frame n
```

Frame `n`'s first LBA is `1 + n * 15`. There is no per-frame header,
palette, or padding -- frames are back-to-back.

## Header (LBA 0, 512 bytes)

| Offset | Size | Field          | Notes                                              |
|-------:|-----:|----------------|-----------------------------------------------------|
| 0      | 4    | `magic`        | ASCII `"CIN2"` (0x43 0x49 0x4E 0x32)                |
| 4      | 1    | `version`      | `2`                                                  |
| 5      | 1    | `flags`        | reserved, must be `0`                                |
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
checksumming 180 KiB/s of pixel data on an ez80 core is not worth the
CPU budget. A reader that gets a bad header CRC must refuse to play
rather than guess at corrupted `fps_num`/`frame_count`/`width`/`height`
values, since those drive LBA arithmetic.

## Frame payload (15 sectors = 7,680 bytes)

Each frame is `width * height / 2` bytes of packed 4-bit palette indices,
row-major, top-to-bottom, left-to-right. Each byte packs two horizontally
adjacent pixels:

```
byte = (left_pixel_index << 4) | right_pixel_index
```

i.e. the high nibble is the even-x pixel, the low nibble is the odd-x
pixel of the same row. `width` must be even (160 is). There is no
row padding -- row `y` starts immediately at byte `y * width / 2`.

## Resume record (TI AppVar `SSCINEV2`)

Written on exit, read on the "resume?" prompt. 33 bytes, all
little-endian:

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
