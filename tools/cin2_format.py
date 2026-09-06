"""Pure byte-level CIN2 format helpers -- the Python mirror of
src/cin2.c. See docs/CIN2_FORMAT.md for the authoritative binary layout;
if you change either this file or src/cin2.c, update the other and the
spec in the same commit. tests/test_cin2_format.py checks this module
against known-answer test vectors shared with tests/test_cin2.c.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field
from typing import Optional, Sequence, Tuple

MAGIC = b"CIN2"
VERSION = 2
HEADER_BYTES = 512
CRC_BYTES = 22  # bytes [0, 22) covered by header_crc32
DATA_LBA = 1
FRAME_SECTORS = 30
SECTOR_BYTES = 512
PALETTE_ENTRIES = 16

# The only geometry the calculator-side player (src/player_v2.c) knows
# how to draw. An encoder targeting a different resolution would need a
# corresponding change on the calculator, not just here.
WIDTH = 160
HEIGHT = 96
# One byte per pixel (0..15, indexing the shared palette), not bit-packed
# -- see docs/CIN2_FORMAT.md's "Why a new format" section for why v2
# dropped 4-bit packing after real-hardware testing showed the CPU cost
# of unpacking it back out cost more than the packing saved.
FRAME_BYTES = WIDTH * HEIGHT
assert FRAME_SECTORS * SECTOR_BYTES == FRAME_BYTES

RESUME_MAGIC = b"CR2S"
RESUME_BYTES = 33
RESUME_FILENAME_LEN = 13


def crc32(data: bytes) -> int:
    """Standard CRC-32 (IEEE 802.3): poly 0xEDB88320, init/final XOR
    0xFFFFFFFF -- the zlib/gzip/PNG variant, which is exactly what
    zlib.crc32 computes."""
    return zlib.crc32(data) & 0xFFFFFFFF


def frame_lba(frame_number: int) -> int:
    return DATA_LBA + frame_number * FRAME_SECTORS


def frame_count_fits_drive(frame_count: int, drive_sectors: int) -> bool:
    """Mirrors src/cin2.c's cin2_frame_count_fits_drive: true if the
    header sector plus every frame's FRAME_SECTORS sectors fits within
    a drive of drive_sectors total logical blocks."""
    required_sectors = DATA_LBA + frame_count * FRAME_SECTORS
    return required_sectors <= drive_sectors


@dataclass
class Cin2Header:
    width: int
    height: int
    fps_num: int
    fps_den: int
    frame_count: int
    palette: Sequence[int] = field(default_factory=lambda: [0] * PALETTE_ENTRIES)


def build_header(header: Cin2Header) -> bytes:
    if len(header.palette) != PALETTE_ENTRIES:
        raise ValueError(f"palette must have exactly {PALETTE_ENTRIES} entries")
    for entry in header.palette:
        if not 0 <= entry <= 0xFFFF:
            raise ValueError("palette entries must fit in 16 bits (RGB1555)")

    buf = bytearray(HEADER_BYTES)
    buf[0:4] = MAGIC
    buf[4] = VERSION
    buf[5] = 0  # flags, reserved
    struct.pack_into(
        "<HHLLL", buf, 6,
        header.width, header.height, header.fps_num, header.fps_den,
        header.frame_count,
    )
    struct.pack_into("<L", buf, 22, crc32(bytes(buf[:CRC_BYTES])))
    for i, entry in enumerate(header.palette):
        struct.pack_into("<H", buf, 26 + i * 2, entry)
    return bytes(buf)


def parse_header(raw: bytes) -> Cin2Header:
    if len(raw) < HEADER_BYTES:
        raise ValueError("header shorter than CIN2_HEADER_BYTES")
    if raw[0:4] != MAGIC:
        raise ValueError("bad magic")
    if raw[4] != VERSION:
        raise ValueError(f"unsupported version {raw[4]}")

    stored_crc = struct.unpack_from("<L", raw, 22)[0]
    computed_crc = crc32(raw[:CRC_BYTES])
    if stored_crc != computed_crc:
        raise ValueError(
            f"bad header CRC (stored 0x{stored_crc:08X}, computed 0x{computed_crc:08X})"
        )

    width, height, fps_num, fps_den, frame_count = struct.unpack_from("<HHLLL", raw, 6)
    if fps_num == 0 or fps_den == 0:
        raise ValueError("fps_num/fps_den must be nonzero")
    palette = list(struct.unpack_from(f"<{PALETTE_ENTRIES}H", raw, 26))

    return Cin2Header(
        width=width, height=height, fps_num=fps_num, fps_den=fps_den,
        frame_count=frame_count, palette=palette,
    )


def encode_frame(indices: Sequence[int]) -> bytes:
    """indices: WIDTH*HEIGHT palette indices (0..15), row-major,
    top-to-bottom, left-to-right. Returns FRAME_BYTES bytes: one byte per
    pixel, unpacked -- this is exactly what lands in the calculator's
    sprite buffer straight off the USB read, with no decode step."""
    if len(indices) != WIDTH * HEIGHT:
        raise ValueError(f"expected {WIDTH * HEIGHT} indices, got {len(indices)}")
    if any(not (0 <= i <= 15) for i in indices):
        raise ValueError("palette indices must be 0..15 (only 16 palette entries exist)")
    return bytes(indices)


def decode_frame(raw: bytes) -> list[int]:
    if len(raw) != FRAME_BYTES:
        raise ValueError(f"expected {FRAME_BYTES} bytes, got {len(raw)}")
    return list(raw)


@dataclass
class ResumeRecord:
    frame_count: int
    last_presented_frame: int
    # NUL-terminated short filename this resume position applies to, or ""
    # for the raw whole-device-image mode. Needed because a FAT32 drive can
    # hold more than one movie -- frame_count alone isn't enough to tell
    # whether a saved resume position belongs to the file currently being
    # opened (two different movies could coincidentally share a frame count).
    filename: str = ""


def build_resume_record(record: ResumeRecord) -> bytes:
    name_bytes = record.filename.encode("ascii")[: RESUME_FILENAME_LEN - 1]

    buf = bytearray(RESUME_BYTES)
    buf[0:4] = RESUME_MAGIC
    buf[4] = VERSION
    struct.pack_into("<L", buf, 8, record.frame_count)
    struct.pack_into("<L", buf, 12, record.last_presented_frame)
    buf[16:16 + len(name_bytes)] = name_bytes
    struct.pack_into("<L", buf, 29, crc32(bytes(buf[:29])))
    return bytes(buf)


def parse_resume_record(raw: bytes) -> ResumeRecord:
    if len(raw) < RESUME_BYTES:
        raise ValueError("resume record shorter than CIN2_RESUME_BYTES")
    if raw[0:4] != RESUME_MAGIC:
        raise ValueError("bad resume magic")
    if raw[4] != VERSION:
        raise ValueError(f"unsupported resume version {raw[4]}")

    stored_crc = struct.unpack_from("<L", raw, 29)[0]
    computed_crc = crc32(raw[:29])
    if stored_crc != computed_crc:
        raise ValueError("bad resume record CRC")

    frame_count, last_presented_frame = struct.unpack_from("<LL", raw, 8)
    name_field = bytes(raw[16:16 + RESUME_FILENAME_LEN - 1])
    filename = name_field.split(b"\x00", 1)[0].decode("ascii")
    return ResumeRecord(
        frame_count=frame_count,
        last_presented_frame=last_presented_frame,
        filename=filename,
    )


# --- multi-slot resume store: mirrors src/cin2.c's cin2_resume_store_*
# functions. See docs/CIN2_FORMAT.md for why more than one slot exists
# (a FAT32 drive can hold several movies, each with its own resume
# position). ---

RESUME_SLOT_COUNT = 8
RESUME_STORE_BYTES = RESUME_BYTES * RESUME_SLOT_COUNT


def resume_store_find(raw: bytes, filename: str) -> Optional[Tuple[int, ResumeRecord]]:
    """Searches a RESUME_STORE_BYTES-long store for a valid slot whose
    filename matches. Returns (slot_index, record), or None if no slot
    matches -- including an all-zero store (never written) or a
    single-slot store from an older build (too short to index at all)."""
    for i in range(RESUME_SLOT_COUNT):
        start = i * RESUME_BYTES
        try:
            record = parse_resume_record(raw[start:start + RESUME_BYTES])
        except ValueError:
            continue
        if record.filename == filename:
            return i, record
    return None


def resume_store_slot_for(raw: bytes, filename: str) -> int:
    """Picks which slot a new resume record for filename should be
    written into: the slot already holding filename if one exists, else
    the first invalid (empty/corrupt) slot, else slot 0."""
    first_invalid = -1
    for i in range(RESUME_SLOT_COUNT):
        start = i * RESUME_BYTES
        try:
            record = parse_resume_record(raw[start:start + RESUME_BYTES])
        except ValueError:
            if first_invalid < 0:
                first_invalid = i
            continue
        if record.filename == filename:
            return i
    return first_invalid if first_invalid >= 0 else 0


def resume_store_write_slot(raw: bytearray, slot: int, record: ResumeRecord) -> None:
    """Writes record into slot `slot` of the RESUME_STORE_BYTES-long
    mutable store, in place."""
    start = slot * RESUME_BYTES
    raw[start:start + RESUME_BYTES] = build_resume_record(record)
