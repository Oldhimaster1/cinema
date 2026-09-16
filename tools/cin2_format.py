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

PLACEMENT_MAGIC = b"CPE1"
PLACEMENT_VERSION = 1
PLACEMENT_OFFSET = 64
PLACEMENT_EXTENTS_OFFSET = 128
PLACEMENT_MAX_EXTENTS = 32
PLACEMENT_FLAG_PREPARED = 0x01
PLACEMENT_KNOWN_FLAGS = PLACEMENT_FLAG_PREPARED

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


@dataclass(frozen=True)
class PlacementExtent:
    start_lba: int
    sector_count: int


@dataclass
class PlacementDescriptor:
    flags: int = PLACEMENT_FLAG_PREPARED
    logical_sector_bytes: int = 512
    sectors_per_cluster: int = 0
    volume_serial: int = 0
    partition_base_lba: int = 0
    first_fat_lba: int = 0
    first_data_lba: int = 0
    fat_size_sectors: int = 0
    total_data_clusters: int = 0
    movie_first_cluster: int = 0
    movie_file_size: int = 0
    total_movie_sectors: int = 0
    immutable_header_crc: int = 0
    extents: Tuple[PlacementExtent, ...] = field(default_factory=tuple)


@dataclass(frozen=True)
class PlacementContext:
    logical_sector_bytes: int
    sectors_per_cluster: int
    volume_serial: int
    partition_base_lba: int
    first_fat_lba: int
    first_data_lba: int
    fat_size_sectors: int
    total_data_clusters: int
    movie_first_cluster: int
    movie_file_size: int
    immutable_header_crc: int


def build_placement(header: bytes, d: PlacementDescriptor) -> bytes:
    if len(header) != HEADER_BYTES:
        raise ValueError("CIN2 header must be exactly 512 bytes")
    if not 1 <= len(d.extents) <= PLACEMENT_MAX_EXTENTS:
        raise ValueError("placement extent count out of range")
    if d.logical_sector_bytes != 512 or d.sectors_per_cluster <= 0:
        raise ValueError("unsupported placement geometry")
    raw = bytearray(header)
    raw[PLACEMENT_OFFSET:] = bytes(HEADER_BYTES - PLACEMENT_OFFSET)
    p = PLACEMENT_OFFSET
    descriptor_bytes = 64 + len(d.extents) * 8
    struct.pack_into("<4sBBHHHB3x10I", raw, p,
                     PLACEMENT_MAGIC, PLACEMENT_VERSION,
                     d.flags | PLACEMENT_FLAG_PREPARED, len(d.extents),
                     descriptor_bytes, d.logical_sector_bytes,
                     d.sectors_per_cluster, d.volume_serial,
                     d.partition_base_lba, d.first_fat_lba,
                     d.first_data_lba, d.fat_size_sectors,
                     d.total_data_clusters, d.movie_first_cluster,
                     d.movie_file_size, d.total_movie_sectors,
                     d.immutable_header_crc)
    for i, extent in enumerate(d.extents):
        struct.pack_into("<II", raw, PLACEMENT_EXTENTS_OFFSET + i * 8,
                         extent.start_lba, extent.sector_count)
    struct.pack_into("<I", raw, p + 56,
                     crc32(raw[PLACEMENT_EXTENTS_OFFSET:
                               PLACEMENT_EXTENTS_OFFSET + len(d.extents) * 8]))
    struct.pack_into("<I", raw, p + 60, crc32(raw[p:p + 60]))
    return bytes(raw)


def parse_placement(header: bytes) -> Optional[PlacementDescriptor]:
    if len(header) < HEADER_BYTES:
        return None
    p = PLACEMENT_OFFSET
    magic, version, flags, count, size, sector_bytes, spc = struct.unpack_from(
        "<4sBBHHHB", header, p)
    if (magic != PLACEMENT_MAGIC or version != PLACEMENT_VERSION
            or flags & ~PLACEMENT_KNOWN_FLAGS
            or not flags & PLACEMENT_FLAG_PREPARED
            or not 1 <= count <= PLACEMENT_MAX_EXTENTS
            or size != 64 + count * 8
            or p + size > HEADER_BYTES
            or sector_bytes != 512 or spc == 0):
        return None
    extent_crc, metadata_crc = struct.unpack_from("<II", header, p + 56)
    if metadata_crc != crc32(header[p:p + 60]):
        return None
    extent_raw = header[PLACEMENT_EXTENTS_OFFSET:PLACEMENT_EXTENTS_OFFSET + count * 8]
    if extent_crc != crc32(extent_raw):
        return None
    values = struct.unpack_from("<10I", header, p + 16)
    extents = tuple(PlacementExtent(*struct.unpack_from("<II", extent_raw, i * 8))
                    for i in range(count))
    return PlacementDescriptor(flags=flags, logical_sector_bytes=sector_bytes,
        sectors_per_cluster=spc, volume_serial=values[0],
        partition_base_lba=values[1], first_fat_lba=values[2],
        first_data_lba=values[3], fat_size_sectors=values[4],
        total_data_clusters=values[5], movie_first_cluster=values[6],
        movie_file_size=values[7], total_movie_sectors=values[8],
        immutable_header_crc=values[9], extents=extents)


def validate_placement(d: PlacementDescriptor, c: PlacementContext) -> bool:
    if (d.logical_sector_bytes != c.logical_sector_bytes
            or d.sectors_per_cluster != c.sectors_per_cluster
            or d.volume_serial != c.volume_serial
            or d.partition_base_lba != c.partition_base_lba
            or d.first_fat_lba != c.first_fat_lba
            or d.first_data_lba != c.first_data_lba
            or d.fat_size_sectors != c.fat_size_sectors
            or d.total_data_clusters != c.total_data_clusters
            or d.movie_first_cluster != c.movie_first_cluster
            or d.movie_file_size != c.movie_file_size
            or d.immutable_header_crc != c.immutable_header_crc
            or c.logical_sector_bytes != 512 or c.sectors_per_cluster <= 0
            or c.movie_first_cluster < 2
            or not 1 <= len(d.extents) <= PLACEMENT_MAX_EXTENTS):
        return False
    expected_sectors = (c.movie_file_size + 511) // 512
    if d.total_movie_sectors != expected_sectors:
        return False
    first_lba = c.first_data_lba + (c.movie_first_cluster - 2) * c.sectors_per_cluster
    if first_lba > 0xFFFFFFFF or d.extents[0].start_lba != first_lba:
        return False
    data_end = c.first_data_lba + c.total_data_clusters * c.sectors_per_cluster
    ranges = []
    total = 0
    for e in d.extents:
        end = e.start_lba + e.sector_count
        if (e.sector_count <= 0 or e.start_lba < c.first_data_lba
                or end > data_end or end > 0x100000000):
            return False
        if any(e.start_lba < b and a < end for a, b in ranges):
            return False
        ranges.append((e.start_lba, end))
        total += e.sector_count
    return total == d.total_movie_sectors


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
