"""Tests for tools/cin2_format.py -- the Python mirror of src/cin2.c.
Known-answer CRC vectors here match the ones in tests/test_decode.c so
both implementations are checked against the same ground truth."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import cin2_format as fmt  # noqa: E402


def test_crc32_known_vectors():
    assert fmt.crc32(b"") == 0x00000000
    assert fmt.crc32(b"123456789") == 0xCBF43926


def test_header_round_trip():
    header = fmt.Cin2Header(
        width=160, height=96, fps_num=24000, fps_den=1001, frame_count=123456,
        palette=[i * 0x1111 & 0xFFFF for i in range(16)],
    )
    raw = fmt.build_header(header)

    assert len(raw) == fmt.HEADER_BYTES
    assert raw[0:4] == fmt.MAGIC

    parsed = fmt.parse_header(raw)
    assert parsed.width == header.width
    assert parsed.height == header.height
    assert parsed.fps_num == header.fps_num
    assert parsed.fps_den == header.fps_den
    assert parsed.frame_count == header.frame_count
    assert list(parsed.palette) == list(header.palette)


def test_header_rejects_corruption():
    header = fmt.Cin2Header(width=160, height=96, fps_num=24, fps_den=1,
                             frame_count=10, palette=[0] * 16)
    raw = bytearray(fmt.build_header(header))

    for i in range(fmt.CRC_BYTES):
        corrupt = bytearray(raw)
        corrupt[i] ^= 0xFF
        with pytest.raises(ValueError):
            fmt.parse_header(bytes(corrupt))


def test_header_rejects_zero_fps():
    header = fmt.Cin2Header(width=160, height=96, fps_num=0, fps_den=1,
                             frame_count=10, palette=[0] * 16)
    raw = fmt.build_header(header)
    with pytest.raises(ValueError):
        fmt.parse_header(raw)


def test_v1_drive_not_misdetected():
    # A v1 drive's LBA 0 is a raw 256-entry RGB565 palette, essentially
    # arbitrary bytes -- confirm a plausible one doesn't collide with
    # the CIN2 magic.
    raw = bytes((i * 37 + 11) & 0xFF for i in range(fmt.HEADER_BYTES))
    assert raw[0:4] != fmt.MAGIC


def test_pack_unpack_frame_round_trip():
    indices = [(x + y) % 16 for y in range(fmt.HEIGHT) for x in range(fmt.WIDTH)]
    packed = fmt.pack_frame(indices)
    assert len(packed) == fmt.PACKED_BYTES
    assert fmt.unpack_frame(packed) == indices


def test_pack_frame_nibble_order():
    # First pixel pair: left=3, right=12 -> byte 0x3C (see docs/CIN2_FORMAT.md).
    indices = [3, 12] + [0] * (fmt.WIDTH * fmt.HEIGHT - 2)
    packed = fmt.pack_frame(indices)
    assert packed[0] == 0x3C


def test_pack_frame_rejects_wrong_length():
    with pytest.raises(ValueError):
        fmt.pack_frame([0] * 10)


def test_pack_frame_rejects_out_of_range_index():
    indices = [0] * (fmt.WIDTH * fmt.HEIGHT)
    indices[0] = 16  # not representable in 4 bits
    with pytest.raises(ValueError):
        fmt.pack_frame(indices)


def test_frame_lba_matches_spec():
    assert fmt.frame_lba(0) == 1
    assert fmt.frame_lba(1) == 1 + fmt.FRAME_SECTORS
    assert fmt.frame_lba(2) == 1 + 2 * fmt.FRAME_SECTORS


def test_frame_count_fits_drive():
    assert fmt.frame_count_fits_drive(0, 1)
    assert not fmt.frame_count_fits_drive(0, 0)
    assert fmt.frame_count_fits_drive(10, 1 + 10 * fmt.FRAME_SECTORS)
    assert not fmt.frame_count_fits_drive(10, 1 + 10 * fmt.FRAME_SECTORS - 1)
    assert not fmt.frame_count_fits_drive(0xFFFFFFFF, 1000)


def test_resume_record_round_trip():
    record = fmt.ResumeRecord(frame_count=7200, last_presented_frame=3141)
    raw = fmt.build_resume_record(record)
    assert len(raw) == fmt.RESUME_BYTES

    parsed = fmt.parse_resume_record(raw)
    assert parsed.frame_count == record.frame_count
    assert parsed.last_presented_frame == record.last_presented_frame


def test_resume_record_rejects_corruption():
    record = fmt.ResumeRecord(frame_count=100, last_presented_frame=50)
    raw = bytearray(fmt.build_resume_record(record))

    for i in range(fmt.RESUME_BYTES):
        corrupt = bytearray(raw)
        corrupt[i] ^= 0xFF
        with pytest.raises(ValueError):
            fmt.parse_resume_record(bytes(corrupt))
