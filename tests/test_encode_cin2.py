"""Tests for tools/encode_cin2.py.

Two tiers:
  - Pure-function unit tests against synthetic in-memory images (no
    ffmpeg needed).
  - A full CLI smoke test that generates a short synthetic test video
    with ffmpeg's lavfi testsrc source (self-contained -- no external
    video asset required) and encodes it end-to-end, checking the
    output file structurally matches what src/cin2.c / player_v2.c
    expect. Skipped automatically if ffmpeg isn't on PATH.
"""
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import cin2_format as fmt  # noqa: E402
import encode_cin2 as enc  # noqa: E402

HAVE_FFMPEG = shutil.which("ffmpeg") is not None


def test_rgb888_to_rgb1555_known_values():
    assert enc.rgb888_to_rgb1555(0, 0, 0) == 0x0000
    # White: top bit stays unused/0 in 1555 -- 0x7FFF, not 0xFFFF.
    assert enc.rgb888_to_rgb1555(0xFF, 0xFF, 0xFF) == 0x7FFF
    # Pure red: top 5 bits of R, nothing else.
    assert enc.rgb888_to_rgb1555(0xFF, 0, 0) == 0x7C00
    # Pure green: middle 5 bits (not 6 -- this isn't 565).
    assert enc.rgb888_to_rgb1555(0, 0xFF, 0) == 0x03E0
    # Pure blue: bottom 5 bits.
    assert enc.rgb888_to_rgb1555(0, 0, 0xFF) == 0x001F


def _solid_frame(color):
    return Image.new("RGB", (fmt.WIDTH, fmt.HEIGHT), color)


def test_build_global_palette_has_16_entries():
    frames = [
        _solid_frame((255, 0, 0)),
        _solid_frame((0, 255, 0)),
        _solid_frame((0, 0, 255)),
        _solid_frame((255, 255, 0)),
    ]
    palette_image = enc.build_global_palette(frames, sample_count=4)
    entries = enc.palette_image_to_rgb1555(palette_image)
    assert len(entries) == fmt.PALETTE_ENTRIES
    assert all(0 <= e <= 0xFFFF for e in entries)


def test_quantize_frame_produces_valid_indices():
    frames = [_solid_frame((255, 0, 0)), _solid_frame((0, 0, 255))]
    palette_image = enc.build_global_palette(frames, sample_count=2)

    indices = enc.quantize_frame(_solid_frame((255, 0, 0)), palette_image)
    assert len(indices) == fmt.WIDTH * fmt.HEIGHT
    assert all(0 <= i <= 15 for i in indices)


def test_quantize_frame_resizes_mismatched_input():
    frames = [_solid_frame((10, 20, 30))]
    palette_image = enc.build_global_palette(frames, sample_count=1)
    big_frame = Image.new("RGB", (320, 240), (10, 20, 30))

    indices = enc.quantize_frame(big_frame, palette_image)
    assert len(indices) == fmt.WIDTH * fmt.HEIGHT


def test_encoded_frame_round_trips_through_encode_and_decode_math():
    """A solid-color frame, quantized against a palette that contains
    its exact color, should encode/decode back to a uniform index -- i.e.
    the whole pipeline (quantize -> encode_frame -> decode_frame) doesn't
    corrupt or shuffle pixels."""
    frames = [_solid_frame((200, 40, 40)), _solid_frame((40, 200, 40))]
    palette_image = enc.build_global_palette(frames, sample_count=2)

    indices = enc.quantize_frame(_solid_frame((200, 40, 40)), palette_image)
    encoded = fmt.encode_frame(indices)
    decoded = fmt.decode_frame(encoded)

    assert decoded == indices
    # A pure solid-color frame quantized against a palette built from
    # (in part) that exact color should be uniform, not speckled --
    # dithering against a single flat input has nothing to dither.
    assert len(set(indices)) == 1


@pytest.mark.skipif(not HAVE_FFMPEG, reason="ffmpeg not on PATH")
def test_full_cli_encode_smoke(tmp_path):
    video_path = tmp_path / "testsrc.mp4"
    output_path = tmp_path / "movie.bin"

    # Self-contained synthetic test video: no external asset needed.
    subprocess.run(
        [
            "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
            "-f", "lavfi", "-i", "testsrc=duration=2:size=320x240:rate=24",
            str(video_path),
        ],
        check=True,
    )

    rc = enc.main([str(video_path), str(output_path), "--fps", "24",
                   "--palette-samples", "8"])
    assert rc == 0
    assert output_path.exists()

    raw = output_path.read_bytes()
    header = fmt.parse_header(raw[: fmt.HEADER_BYTES])

    assert header.width == fmt.WIDTH
    assert header.height == fmt.HEIGHT
    assert header.fps_num == 24
    assert header.fps_den == 1
    # ~2 seconds @ 24fps => ~48 frames; ffmpeg's fps filter can be off
    # by a frame or two at clip boundaries.
    assert 44 <= header.frame_count <= 52

    expected_size = fmt.HEADER_BYTES + header.frame_count * fmt.FRAME_BYTES
    assert len(raw) == expected_size

    # Every frame's bytes must exist and be exactly FRAME_BYTES long --
    # i.e. frames are laid out back-to-back with no gaps, matching
    # cin2_frame_lba()'s fixed stride.
    for frame_number in range(header.frame_count):
        start = fmt.frame_lba(frame_number) * fmt.SECTOR_BYTES
        end = start + fmt.FRAME_BYTES
        assert end <= len(raw)
        indices = fmt.decode_frame(raw[start:end])
        assert all(0 <= i <= 15 for i in indices)


@pytest.mark.skipif(not HAVE_FFMPEG, reason="ffmpeg not on PATH")
def test_encode_is_deterministic(tmp_path):
    video_path = tmp_path / "testsrc.mp4"
    subprocess.run(
        ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
         "-f", "lavfi", "-i", "testsrc2=duration=1:size=320x240:rate=24",
         str(video_path)],
        check=True,
    )

    out1 = tmp_path / "run1.bin"
    out2 = tmp_path / "run2.bin"
    enc.main([str(video_path), str(out1), "--fps", "24", "--palette-samples", "8"])
    enc.main([str(video_path), str(out2), "--fps", "24", "--palette-samples", "8"])

    raw1, raw2 = out1.read_bytes(), out2.read_bytes()
    assert raw1 == raw2, "two encodes of the same input produced different bytes"
    import hashlib
    assert hashlib.sha256(raw1).hexdigest() == hashlib.sha256(raw2).hexdigest()


@pytest.mark.skipif(not HAVE_FFMPEG, reason="ffmpeg not on PATH")
def test_partial_file_never_left_under_final_name_on_failure(tmp_path):
    """A video ffmpeg can't decode (garbage bytes) must fail cleanly:
    no file at the requested output path, and no leftover .partial file
    either -- see encode()'s try/except in tools/encode_cin2.py."""
    bad_video = tmp_path / "not_a_video.mp4"
    bad_video.write_bytes(b"this is not a real video file")
    output_path = tmp_path / "movie.bin"

    with pytest.raises(subprocess.CalledProcessError):
        enc.main([str(bad_video), str(output_path), "--fps", "24"])

    assert not output_path.exists(), "output must not exist after a failed encode"
    assert not (tmp_path / "movie.bin.partial").exists(), "partial file must be cleaned up"


@pytest.mark.skipif(not HAVE_FFMPEG, reason="ffmpeg not on PATH")
def test_existing_output_untouched_if_new_encode_fails(tmp_path):
    """Re-encoding into a path that already holds a valid (perhaps
    older) CIN2 file must not clobber it if the new encode fails."""
    video_path = tmp_path / "testsrc.mp4"
    subprocess.run(
        ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
         "-f", "lavfi", "-i", "testsrc2=duration=1:size=320x240:rate=24",
         str(video_path)],
        check=True,
    )
    output_path = tmp_path / "movie.bin"
    assert enc.main([str(video_path), str(output_path), "--fps", "24"]) == 0
    original_bytes = output_path.read_bytes()

    bad_video = tmp_path / "not_a_video.mp4"
    bad_video.write_bytes(b"garbage")
    with pytest.raises(subprocess.CalledProcessError):
        enc.main([str(bad_video), str(output_path), "--fps", "24"])

    assert output_path.read_bytes() == original_bytes, \
        "existing valid output was overwritten by a failed re-encode"


@pytest.mark.skipif(not HAVE_FFMPEG, reason="ffmpeg not on PATH")
def test_cli_rejects_nonpositive_fps(tmp_path, capsys):
    video_path = tmp_path / "in.mp4"
    video_path.write_bytes(b"not a real video, but argparse should fail first")
    output_path = tmp_path / "out.bin"

    with pytest.raises(SystemExit):
        enc.main([str(video_path), str(output_path), "--fps", "0"])


@pytest.mark.skipif(not HAVE_FFMPEG, reason="ffmpeg not on PATH")
def test_batch_encode_directory_produces_one_bin_per_video(tmp_path):
    in_dir = tmp_path / "videos"
    out_dir = tmp_path / "encoded"
    in_dir.mkdir()

    for name in ("a", "b"):
        subprocess.run(
            ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
             "-f", "lavfi", "-i", "testsrc=duration=1:size=320x240:rate=15",
             str(in_dir / f"{name}.mp4")],
            check=True,
        )

    rc = enc.main([str(in_dir), str(out_dir), "--fps", "15", "--palette-samples", "4"])

    assert rc == 0
    assert (out_dir / "a.bin").exists()
    assert (out_dir / "b.bin").exists()
    header = fmt.parse_header((out_dir / "a.bin").read_bytes()[: fmt.HEADER_BYTES])
    assert header.fps_num == 15 and header.fps_den == 1


@pytest.mark.skipif(not HAVE_FFMPEG, reason="ffmpeg not on PATH")
def test_batch_encode_one_bad_file_does_not_sink_the_others(tmp_path):
    in_dir = tmp_path / "videos"
    out_dir = tmp_path / "encoded"
    in_dir.mkdir()

    subprocess.run(
        ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
         "-f", "lavfi", "-i", "testsrc=duration=1:size=320x240:rate=15",
         str(in_dir / "good.mp4")],
        check=True,
    )
    (in_dir / "bad.mp4").write_bytes(b"not actually a video")

    rc = enc.main([str(in_dir), str(out_dir), "--fps", "15", "--palette-samples", "4"])

    assert rc == 1, "at least one failure is reported via a nonzero exit code"
    assert (out_dir / "good.bin").exists(), "the good file still encoded despite the bad one"
    assert not (out_dir / "bad.bin").exists()


def test_batch_encode_empty_directory_reports_no_matches(tmp_path):
    in_dir = tmp_path / "empty"
    in_dir.mkdir()

    rc = enc.main([str(in_dir), str(tmp_path / "out")])

    assert rc == 2


def test_palette_sampling_uses_complete_selected_duration(monkeypatch, tmp_path):
    calls = []
    red = _solid_frame((255, 0, 0))
    blue = _solid_frame((0, 0, 255))

    monkeypatch.setattr(enc, "probe_duration_seconds", lambda path: 600.0)

    def fake_stream(path, fps_num, fps_den, start, duration, max_frames=None):
        calls.append((duration, max_frames))
        if max_frames is not None:
            yield red
            yield blue
        else:
            yield red

    monkeypatch.setattr(enc, "stream_raw_frames", fake_stream)
    out = tmp_path / "out.bin"
    enc.encode(tmp_path / "input.mp4", out, 1, 1, 2, None, 1.0, jobs=1)
    assert calls[0] == (1.0, 2)


def test_full_movie_palette_span_is_not_capped(monkeypatch, tmp_path):
    spans = []
    frame = _solid_frame((20, 80, 220))
    monkeypatch.setattr(enc, "probe_duration_seconds", lambda path: 180.0)

    def fake_stream(path, fps_num, fps_den, start, duration, max_frames=None):
        spans.append((duration, max_frames))
        count = 2 if max_frames is not None else 180
        for _ in range(count):
            yield frame

    monkeypatch.setattr(enc, "stream_raw_frames", fake_stream)
    out = tmp_path / "movie.bin"
    enc.encode(tmp_path / "input.mp4", out, 1, 1, 2, None, None, jobs=1)
    assert spans[0] == (180.0, 2)
    header = fmt.parse_header(out.read_bytes()[: fmt.HEADER_BYTES])
    assert header.frame_count == 180


@pytest.mark.skipif(not HAVE_FFMPEG, reason="ffmpeg not on PATH")
def test_30_to_20_fps_preserves_motion_duration(tmp_path):
    video_path = tmp_path / "timing.mp4"
    output_path = tmp_path / "timing.bin"
    subprocess.run([
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "testsrc2=duration=6:size=320x240:rate=30",
        str(video_path),
    ], check=True)
    enc.main([str(video_path), str(output_path), "--fps", "20",
              "--palette-samples", "8", "--jobs", "1"])
    header = fmt.parse_header(output_path.read_bytes()[: fmt.HEADER_BYTES])
    encoded_duration = header.frame_count * header.fps_den / header.fps_num
    assert abs(encoded_duration - 6.0) <= 2.0 / 20.0
    assert 118 <= header.frame_count <= 122


def test_rgb1555_roundtrip_display_endpoints():
    assert enc.rgb1555_to_rgb888(0x0000) == (0, 0, 0)
    assert enc.rgb1555_to_rgb888(0x7FFF) == (255, 255, 255)
    for value in (0x7C00, 0x03E0, 0x001F, 0x4210, 0x1234):
        assert enc.rgb888_to_rgb1555(*enc.rgb1555_to_rgb888(value)) == value


def test_palette_image_contains_exact_rgb1555_display_colors():
    frames = [enc.Image.new("RGB", (enc.WIDTH, enc.HEIGHT), (i * 16, 255 - i * 12, i * 9))
              for i in range(16)]
    pal = enc.build_global_palette(frames, 16)
    entries = enc.palette_image_to_rgb1555(pal)
    raw = pal.getpalette()
    for i, value in enumerate(entries):
        assert tuple(raw[i * 3:i * 3 + 3]) == enc.rgb1555_to_rgb888(value)


def test_palette_uses_16_distinct_rgb1555_entries_when_source_is_diverse():
    frame = enc.Image.new("RGB", (enc.WIDTH, enc.HEIGHT))
    colors = [(r, g, b) for r in (0, 85, 170, 255)
                        for g in (0, 85, 170, 255)
                        for b in (0, 255)]
    pixels = [colors[(x // 10 + y // 8 * 16) % len(colors)]
              for y in range(enc.HEIGHT) for x in range(enc.WIDTH)]
    frame.putdata(pixels)
    entries = enc.palette_image_to_rgb1555(enc.build_global_palette([frame], 1))
    assert len(set(entries)) == 16


def test_clean_dither_removes_speckles_from_flat_surface():
    colors = [(64, 64, 64), (192, 192, 192)]
    pal = enc.build_global_palette([_solid_frame(c) for c in colors], 2)
    frame = _solid_frame((120, 120, 120))
    clean = enc.quantize_frame(frame, pal, "clean")
    assert len(set(clean)) == 1


def test_unknown_dither_mode_rejected():
    pal = enc.build_global_palette([_solid_frame((0, 0, 0))], 1)
    with pytest.raises(ValueError):
        enc.quantize_frame(_solid_frame((0, 0, 0)), pal, "bogus")


def test_automatic_palette_sample_policy():
    assert enc.automatic_palette_samples(60) == 32
    assert enc.automatic_palette_samples(600) == 64
    assert enc.automatic_palette_samples(3600) == 128
    assert enc.automatic_palette_samples(7000) == 256
    assert enc.automatic_palette_samples(None) == 64
