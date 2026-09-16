"""Tests for tools/cin2_format.py -- the Python mirror of src/cin2.c.
Known-answer CRC vectors here match the ones in tests/test_cin2.c so
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


def test_encode_decode_frame_round_trip():
    indices = [(x + y) % 16 for y in range(fmt.HEIGHT) for x in range(fmt.WIDTH)]
    encoded = fmt.encode_frame(indices)
    assert len(encoded) == fmt.FRAME_BYTES
    assert fmt.decode_frame(encoded) == indices


def test_encode_frame_is_one_byte_per_pixel():
    # No packing at all: byte i is exactly index i (see docs/CIN2_FORMAT.md
    # -- v2 dropped 4-bit packing since the real cost was the unpack step
    # this used to require on the calculator, not the extra I/O).
    indices = [3, 12] + [0] * (fmt.WIDTH * fmt.HEIGHT - 2)
    encoded = fmt.encode_frame(indices)
    assert encoded[0] == 3
    assert encoded[1] == 12


def test_encode_frame_rejects_wrong_length():
    with pytest.raises(ValueError):
        fmt.encode_frame([0] * 10)


def test_encode_frame_rejects_out_of_range_index():
    indices = [0] * (fmt.WIDTH * fmt.HEIGHT)
    indices[0] = 16  # only 16 palette entries (0..15) exist
    with pytest.raises(ValueError):
        fmt.encode_frame(indices)


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
    record = fmt.ResumeRecord(frame_count=7200, last_presented_frame=3141,
                               filename="MOVIE01.BIN")
    raw = fmt.build_resume_record(record)
    assert len(raw) == fmt.RESUME_BYTES

    parsed = fmt.parse_resume_record(raw)
    assert parsed.frame_count == record.frame_count
    assert parsed.last_presented_frame == record.last_presented_frame
    assert parsed.filename == record.filename


def test_resume_record_round_trip_empty_filename():
    # "" is the raw whole-device-image mode's identity.
    record = fmt.ResumeRecord(frame_count=42, last_presented_frame=10, filename="")
    raw = fmt.build_resume_record(record)

    parsed = fmt.parse_resume_record(raw)
    assert parsed.filename == ""


def test_resume_record_truncates_long_filename():
    # RESUME_FILENAME_LEN is 13 (8.3 short name + NUL); anything longer
    # must be truncated rather than overflowing the fixed-size field.
    long_name = "THISNAMEISWAYTOOLONG.BIN"
    record = fmt.ResumeRecord(frame_count=1, last_presented_frame=0, filename=long_name)
    raw = fmt.build_resume_record(record)

    parsed = fmt.parse_resume_record(raw)
    assert parsed.filename == long_name[: fmt.RESUME_FILENAME_LEN - 1]


def test_resume_record_rejects_corruption():
    record = fmt.ResumeRecord(frame_count=100, last_presented_frame=50)
    raw = bytearray(fmt.build_resume_record(record))

    for i in range(fmt.RESUME_BYTES):
        corrupt = bytearray(raw)
        corrupt[i] ^= 0xFF
        with pytest.raises(ValueError):
            fmt.parse_resume_record(bytes(corrupt))


def test_resume_store_empty_store_finds_nothing():
    store = bytes(fmt.RESUME_STORE_BYTES)
    assert fmt.resume_store_find(store, "MOVIE.BIN") is None


def test_resume_store_write_then_find():
    store = bytearray(fmt.RESUME_STORE_BYTES)
    record = fmt.ResumeRecord(frame_count=500, last_presented_frame=42, filename="MOVIE1.BIN")

    slot = fmt.resume_store_slot_for(store, record.filename)
    assert slot == 0
    fmt.resume_store_write_slot(store, slot, record)

    found = fmt.resume_store_find(store, "MOVIE1.BIN")
    assert found is not None
    assert found[0] == slot
    assert found[1].frame_count == record.frame_count
    assert found[1].last_presented_frame == record.last_presented_frame
    assert fmt.resume_store_find(store, "OTHER.BIN") is None


def test_resume_store_multiple_movies_coexist():
    store = bytearray(fmt.RESUME_STORE_BYTES)
    a = fmt.ResumeRecord(frame_count=100, last_presented_frame=10, filename="A.BIN")
    b = fmt.ResumeRecord(frame_count=200, last_presented_frame=20, filename="B.BIN")

    slot_a = fmt.resume_store_slot_for(store, a.filename)
    fmt.resume_store_write_slot(store, slot_a, a)
    slot_b = fmt.resume_store_slot_for(store, b.filename)
    assert slot_b != slot_a
    fmt.resume_store_write_slot(store, slot_b, b)

    found_a = fmt.resume_store_find(store, "A.BIN")
    found_b = fmt.resume_store_find(store, "B.BIN")
    assert found_a == (slot_a, a)
    assert found_b == (slot_b, b)


def test_resume_store_rewatching_reuses_same_slot():
    store = bytearray(fmt.RESUME_STORE_BYTES)
    record = fmt.ResumeRecord(frame_count=300, last_presented_frame=5, filename="MOVIE.BIN")

    first_slot = fmt.resume_store_slot_for(store, record.filename)
    fmt.resume_store_write_slot(store, first_slot, record)

    record.last_presented_frame = 150
    second_slot = fmt.resume_store_slot_for(store, record.filename)
    assert second_slot == first_slot
    fmt.resume_store_write_slot(store, second_slot, record)

    found = fmt.resume_store_find(store, "MOVIE.BIN")
    assert found == (first_slot, record)


def test_resume_store_fills_then_evicts_slot_zero():
    store = bytearray(fmt.RESUME_STORE_BYTES)

    for i in range(fmt.RESUME_SLOT_COUNT):
        record = fmt.ResumeRecord(frame_count=100 + i, last_presented_frame=i, filename=f"M{i}.BIN")
        slot = fmt.resume_store_slot_for(store, record.filename)
        assert slot == i
        fmt.resume_store_write_slot(store, slot, record)

    assert fmt.resume_store_slot_for(store, "OVERFLOW.BIN") == 0
    found = fmt.resume_store_find(store, "M0.BIN")
    assert found is not None and found[0] == 0


def _placement_case(extents=(fmt.PlacementExtent(108, 60),)):
    header = fmt.build_header(fmt.Cin2Header(width=160, height=96,
        fps_num=20, fps_den=1, frame_count=2, palette=tuple(range(16))))
    d = fmt.PlacementDescriptor(sectors_per_cluster=4, volume_serial=0x12345678,
        partition_base_lba=0, first_fat_lba=32, first_data_lba=100,
        fat_size_sectors=64, total_data_clusters=10000, movie_first_cluster=4,
        movie_file_size=60 * 512, total_movie_sectors=60,
        immutable_header_crc=0xAABBCCDD, extents=extents)
    c = fmt.PlacementContext(512, 4, 0x12345678, 0, 32, 100, 64,
        10000, 4, 60 * 512, 0xAABBCCDD)
    return header, d, c


def test_placement_roundtrip_one_extent_and_header_unchanged():
    header, d, c = _placement_case()
    prepared = fmt.build_placement(header, d)
    assert prepared[:58] == header[:58]
    parsed = fmt.parse_placement(prepared)
    assert parsed == d
    assert fmt.validate_placement(parsed, c)


def test_placement_roundtrip_multiple_extents():
    header, d, c = _placement_case((fmt.PlacementExtent(108, 20),
                                     fmt.PlacementExtent(500, 40)))
    parsed = fmt.parse_placement(fmt.build_placement(header, d))
    assert parsed is not None and fmt.validate_placement(parsed, c)


def test_placement_absent_unprepared_and_crc_corruption_rejected():
    header, d, _ = _placement_case()
    assert fmt.parse_placement(header) is None
    prepared = bytearray(fmt.build_placement(header, d))
    prepared[fmt.PLACEMENT_OFFSET + 5] = 0
    assert fmt.parse_placement(prepared) is None
    prepared = bytearray(fmt.build_placement(header, d))
    prepared[fmt.PLACEMENT_EXTENTS_OFFSET] ^= 1
    assert fmt.parse_placement(prepared) is None
    prepared = bytearray(fmt.build_placement(header, d))
    prepared[fmt.PLACEMENT_OFFSET + 20] ^= 1
    assert fmt.parse_placement(prepared) is None


def test_placement_validation_identity_range_sum_overlap_and_first_lba():
    from dataclasses import replace
    header, d, c = _placement_case()
    parsed = fmt.parse_placement(fmt.build_placement(header, d))
    assert parsed is not None
    for bad in [replace(c, volume_serial=9), replace(c, movie_first_cluster=5),
                replace(c, movie_file_size=1), replace(c, immutable_header_crc=1),
                replace(c, sectors_per_cluster=8)]:
        assert not fmt.validate_placement(parsed, bad)
    for extents in [
        (fmt.PlacementExtent(109, 60),),
        (fmt.PlacementExtent(108, 59),),
        (fmt.PlacementExtent(99, 60),),
        (fmt.PlacementExtent(108, 30), fmt.PlacementExtent(120, 30)),
        (fmt.PlacementExtent(108, 0),),
    ]:
        assert not fmt.validate_placement(replace(parsed, extents=extents), c)


def test_placement_builder_rejects_extent_count_boundaries():
    header, d, _ = _placement_case()
    import pytest
    with pytest.raises(ValueError):
        fmt.build_placement(header, d.__class__(**{**d.__dict__, "extents": ()}))
    too_many = tuple(fmt.PlacementExtent(108 + i, 1)
                     for i in range(fmt.PLACEMENT_MAX_EXTENTS + 1))
    with pytest.raises(ValueError):
        fmt.build_placement(header, d.__class__(**{**d.__dict__, "extents": too_many}))
