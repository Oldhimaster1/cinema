"""Regressions for large-file safety, selected-duration timing and Packed4.

All media are generated locally by FFmpeg; no user footage is used.
"""
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import cin2_format as fmt  # noqa: E402
import decode_cin2 as dec  # noqa: E402
import encode_cin2 as enc  # noqa: E402
import verify_cin2 as verifier  # noqa: E402
from verify_csu import verify as verify_csu  # noqa: E402


def _video(tmp_path: Path) -> Path:
    source = tmp_path / "synthetic.mp4"
    subprocess.run([
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "testsrc2=duration=2:size=160x96:rate=24",
        "-c:v", "mpeg4", str(source),
    ], check=True)
    return source


def _assert_no_read_bytes(monkeypatch):
    def forbidden(self):
        raise AssertionError(f"unexpected whole-file read: {self}")
    monkeypatch.setattr(Path, "read_bytes", forbidden)


def test_missing_movie_verifier_returns_a_clean_failure(tmp_path):
    missing = tmp_path / "nonexistent.bin"
    result = verifier.verify(missing)
    assert not result["ok"]
    assert result["file"] == str(missing)
    assert "cannot read file" in verifier.format_readable(result)
    assert verifier.main([str(missing)]) == 1


def test_large_file_checks_do_not_require_whole_file_in_memory(tmp_path, monkeypatch):
    path = tmp_path / "oneframe.bin"
    header = fmt.build_header(fmt.Cin2Header(160, 96, 15, 1, 1))
    path.write_bytes(header + bytes(fmt.FRAME_BYTES))

    _assert_no_read_bytes(monkeypatch)
    assert verifier.verify(path)["ok"]
    enc._self_check(path, expected_frame_count=1)
    mapped, parsed = dec.read_movie(path)
    try:
        assert dec.frame_indices(mapped, parsed, 0) == [0] * fmt.FRAME_BYTES
    finally:
        mapped.close()


def test_packed4_nibble_order_and_crc_header():
    indices = [i % 16 for i in range(fmt.FRAME_BYTES)]
    packed = fmt.encode_frame(indices, packed4=True)
    assert len(packed) == fmt.PACKED4_FRAME_BYTES
    assert packed[:4] == bytes((0x01, 0x23, 0x45, 0x67))
    assert fmt.decode_frame(packed, packed4=True) == indices
    header = fmt.Cin2Header(160, 96, 24, 1, 1, flags=fmt.FLAG_PACKED4)
    parsed = fmt.parse_header(fmt.build_header(header))
    assert parsed.flags == fmt.FLAG_PACKED4
    assert fmt.frame_lba(2, parsed.flags) == 31
    assert fmt.frame_count_fits_drive(10, 151, fmt.FLAG_PACKED4)
    assert not fmt.frame_count_fits_drive(10, 150, fmt.FLAG_PACKED4)
    assert not fmt.frame_count_fits_drive(10, 301, 0x80)


@pytest.mark.skipif(not shutil.which("ffmpeg"), reason="FFmpeg is required")
def test_start_offset_without_duration_does_not_fail_timing_check(tmp_path):
    source = _video(tmp_path)
    output = tmp_path / "last_second.bin"
    assert enc.main([str(source), str(output), "--start", "1", "--fps", "15",
                     "--jobs", "1", "--palette-samples", "8"]) == 0
    header = fmt.parse_header(output.open("rb").read(fmt.HEADER_BYTES))
    assert 13 <= header.frame_count <= 17
    assert verifier.verify(output)["ok"]


@pytest.mark.skipif(not shutil.which("ffmpeg"), reason="FFmpeg is required")
def test_packed4_encoding_is_pixel_identical_and_subtitles_survive(tmp_path):
    source = _video(tmp_path)
    raw_path = tmp_path / "RAW.BIN"
    packed_path = tmp_path / "PACKED.BIN"
    srt = tmp_path / "captions.srt"
    srt.write_text("1\n00:00:00,000 --> 00:00:00,900\nHello Cinema!\n\n"
                   "2\n00:00:01,000 --> 00:00:01,900\nSubtitles still work.\n",
                   encoding="utf-8")

    common = ["--fps", "24", "--jobs", "1", "--palette-samples", "8"]
    assert enc.main([str(source), str(raw_path), *common]) == 0
    assert enc.main([str(source), str(packed_path), *common,
                     "--packed4", "--subtitles", str(srt)]) == 0

    raw_bytes = raw_path.read_bytes()
    packed_bytes = packed_path.read_bytes()
    hraw = fmt.parse_header(raw_bytes[:fmt.HEADER_BYTES])
    hpacked = fmt.parse_header(packed_bytes[:fmt.HEADER_BYTES])
    assert hraw.flags == 0
    assert hpacked.flags == fmt.FLAG_PACKED4
    assert hraw.frame_count == hpacked.frame_count
    assert hraw.palette == hpacked.palette
    assert len(packed_bytes) == (len(raw_bytes) + fmt.HEADER_BYTES) // 2
    assert verifier.verify(packed_path)["ok"]
    for i in range(hraw.frame_count):
        assert dec.frame_indices(raw_bytes, hraw, i) == dec.frame_indices(packed_bytes, hpacked, i)

    cues, movie_id = verify_csu(packed_path.with_suffix(".csu"))
    assert cues == 2
    assert movie_id != 0

    if shutil.which("gcc"):
        repo_root = Path(__file__).resolve().parent.parent
        runner = tmp_path / "csu_crosslang"
        subprocess.run([
            "gcc", "-Wall", "-Wextra", "-std=c99", "-Isrc",
            "tests/test_csu_generated_crosslang.c", "src/cin2.c",
            "src/cinema_subtitle.c", "src/cinema_codec.c",
            "-o", str(runner),
        ], cwd=repo_root, check=True)
        subprocess.run([str(runner), str(packed_path),
                        str(packed_path.with_suffix(".csu"))], check=True)


@pytest.mark.skipif(not shutil.which("ffmpeg"), reason="FFmpeg is required")
def test_trimmed_packed_movie_subtitles_are_clipped_and_aligned(tmp_path):
    source = _video(tmp_path)
    movie = tmp_path / "TRIMMED.BIN"
    srt = tmp_path / "original_timestamps.srt"
    srt.write_text(
        "1\n00:00:00,000 --> 00:00:00,500\nBefore clip\n\n"
        "2\n00:00:00,800 --> 00:00:01,200\nAt clip start\n\n"
        "3\n00:00:01,250 --> 00:00:01,600\nIn the clip\n\n"
        "4\n00:00:01,800 --> 00:00:02,300\nAt clip end\n\n"
        "5\n00:00:02,500 --> 00:00:03,000\nAfter clip\n",
        encoding="utf-8",
    )
    assert enc.main([str(source), str(movie), "--fps", "24", "--jobs", "1",
                     "--palette-samples", "8", "--start", "1.0", "--packed4",
                     "--subtitles", str(srt)]) == 0
    header = fmt.parse_header(movie.open("rb").read(fmt.HEADER_BYTES))
    assert header.frame_count == 24
    subtitle = movie.with_suffix(".csu")
    assert verify_csu(subtitle)[0] == 3
    raw = subtitle.read_bytes()
    starts_ends = [struct.unpack_from("<II", raw, 40 + i * 72) for i in range(3)]
    assert starts_ends == [(0, 5), (6, 14), (19, 24)]
    report = json.loads(Path(str(subtitle) + ".report.json").read_text())
    assert report["skipped_cues"] == 2
    assert report["start_offset_ms"] == 1000


def test_batch_mode_does_not_silently_ignore_subtitles(tmp_path):
    with pytest.raises(SystemExit, match="2"):
        enc.main([str(tmp_path), str(tmp_path / "output"),
                  "--subtitles", str(tmp_path / "some.srt")])


@pytest.mark.skipif(not shutil.which("ffmpeg"), reason="FFmpeg is required")
@pytest.mark.parametrize("fps,packed4", [
    ("15", False), ("15", True), ("24", False), ("24", True),
    ("30000/1001", False), ("30000/1001", True),
])
def test_encoding_matrix_for_trimmed_rational_fps_and_storage_modes(
        tmp_path, fps, packed4):
    source = _video(tmp_path)
    movie = tmp_path / "MATRIX.BIN"
    flags = ["--packed4"] if packed4 else []
    assert enc.main([str(source), str(movie), "--fps", fps, "--start", "0.25",
                     "--duration", "1.25", "--jobs", "2",
                     "--palette-samples", "8", *flags]) == 0
    result = verifier.verify(movie)
    assert result["ok"], result["errors"]
    mapped, header = dec.read_movie(movie)
    try:
        assert header.flags == (fmt.FLAG_PACKED4 if packed4 else 0)
        assert abs(header.frame_count * header.fps_den / header.fps_num - 1.25) < 0.08
        assert len(dec.frame_indices(mapped, header, 0)) == fmt.FRAME_BYTES
        assert len(dec.frame_indices(mapped, header, header.frame_count - 1)) == fmt.FRAME_BYTES
    finally:
        mapped.close()


@pytest.mark.skipif(not shutil.which("ffmpeg"), reason="FFmpeg is required")
@pytest.mark.parametrize("packed4", [False, True])
def test_worker_count_does_not_change_encoded_video_bytes(tmp_path, packed4):
    source = _video(tmp_path)
    one = tmp_path / "one.bin"
    four = tmp_path / "four.bin"
    flags = ["--packed4"] if packed4 else []
    common = ["--fps", "24", "--palette-samples", "8", *flags]
    assert enc.main([str(source), str(one), *common, "--jobs", "1"]) == 0
    assert enc.main([str(source), str(four), *common, "--jobs", "4"]) == 0
    assert one.read_bytes() == four.read_bytes()
