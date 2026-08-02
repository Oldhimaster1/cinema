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


def test_rgb888_to_rgb565_known_values():
    assert enc.rgb888_to_rgb565(0, 0, 0) == 0x0000
    assert enc.rgb888_to_rgb565(0xFF, 0xFF, 0xFF) == 0xFFFF
    # Pure red: top 5 bits of R, nothing else.
    assert enc.rgb888_to_rgb565(0xFF, 0, 0) == 0xF800
    # Pure green: middle 6 bits.
    assert enc.rgb888_to_rgb565(0, 0xFF, 0) == 0x07E0
    # Pure blue: bottom 5 bits.
    assert enc.rgb888_to_rgb565(0, 0, 0xFF) == 0x001F


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
    entries = enc.palette_image_to_rgb565(palette_image)
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


def test_encoded_frame_round_trips_through_pack_and_decode_math():
    """A solid-color frame, quantized against a palette that contains
    its exact color, should pack/unpack back to a uniform index -- i.e.
    the whole pipeline (quantize -> pack_frame -> unpack_frame) doesn't
    corrupt or shuffle pixels."""
    frames = [_solid_frame((200, 40, 40)), _solid_frame((40, 200, 40))]
    palette_image = enc.build_global_palette(frames, sample_count=2)

    indices = enc.quantize_frame(_solid_frame((200, 40, 40)), palette_image)
    packed = fmt.pack_frame(indices)
    unpacked = fmt.unpack_frame(packed)

    assert unpacked == indices
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

    expected_size = fmt.HEADER_BYTES + header.frame_count * fmt.PACKED_BYTES
    assert len(raw) == expected_size

    # Every frame's bytes must exist and be exactly PACKED_BYTES long --
    # i.e. frames are laid out back-to-back with no gaps, matching
    # cin2_frame_lba()'s fixed stride.
    for frame_number in range(header.frame_count):
        start = fmt.frame_lba(frame_number) * fmt.SECTOR_BYTES
        end = start + fmt.PACKED_BYTES
        assert end <= len(raw)
        indices = fmt.unpack_frame(raw[start:end])
        assert all(0 <= i <= 15 for i in indices)


@pytest.mark.skipif(not HAVE_FFMPEG, reason="ffmpeg not on PATH")
def test_cli_rejects_nonpositive_fps(tmp_path, capsys):
    video_path = tmp_path / "in.mp4"
    video_path.write_bytes(b"not a real video, but argparse should fail first")
    output_path = tmp_path / "out.bin"

    with pytest.raises(SystemExit):
        enc.main([str(video_path), str(output_path), "--fps", "0"])
