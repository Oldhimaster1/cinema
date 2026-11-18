"""Experimental quality modes must be deterministic and format-compatible."""
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cin2_format as fmt  # noqa: E402
import encode_cin2 as enc  # noqa: E402
import verify_cin2 as verify  # noqa: E402


def _sample_frames():
    frames = []
    for phase in range(4):
        image = Image.new("RGB", (160, 96))
        image.putdata([
            (min(255, x + phase * 15), (x + y * 2) % 256,
             min(255, 70 + y + phase * 20))
            for y in range(96) for x in range(160)
        ])
        frames.append(image)
    return frames


def _weighted_error(frames, palette):
    colors = [enc.rgb1555_to_rgb888(c) for c in enc.palette_image_to_rgb1555(palette)]
    total = 0
    for frame in frames:
        indices = enc.quantize_frame(frame, palette)
        for (r, g, b), idx in zip(frame.get_flattened_data(), indices):
            cr, cg, cb = colors[idx]
            total += 2 * (r - cr) ** 2 + 4 * (g - cg) ** 2 + (b - cb) ** 2
    return total


def test_weighted_palette_refinement_preserves_validity_and_error_goal():
    frames = _sample_frames()
    median = enc.build_global_palette(frames, len(frames))
    improved = enc.refine_palette_weighted(frames, median)
    assert enc.palette_image_to_rgb1555(improved) == enc.palette_image_to_rgb1555(
        enc.refine_palette_weighted(frames, median))
    assert len(enc.palette_image_to_rgb1555(improved)) == 16
    # The quality mode is an opt-in: even its fallback is legal and exact.
    assert _weighted_error(frames, improved) <= _weighted_error(frames, median)


def test_stable_dither_uses_repeatable_spatial_pattern():
    frame = _sample_frames()[0]
    palette = enc.build_global_palette(_sample_frames(), 4)
    one = enc.quantize_frame(frame, palette, "stable")
    two = enc.quantize_frame(frame, palette, "stable")
    assert one == two
    assert len(one) == fmt.FRAME_BYTES
    assert all(0 <= i < 16 for i in one)


@pytest.mark.skipif(not shutil.which("ffmpeg"), reason="FFmpeg required")
@pytest.mark.parametrize("packed", [False, True])
def test_quality_modes_encode_equivalently_across_worker_counts(tmp_path, packed):
    source = tmp_path / "test.mp4"
    subprocess.run([
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "testsrc2=duration=1:size=160x96:rate=12",
        "-c:v", "mpeg4", str(source),
    ], check=True)
    one = tmp_path / "single.BIN"
    four = tmp_path / "multi.BIN"
    common = ["--fps", "12", "--palette-samples", "8", "--dither", "stable",
              "--palette-method", "weighted", "--fit", "contain"]
    if packed:
        common.append("--packed4")
    assert enc.main([str(source), str(one), *common, "--jobs", "1"]) == 0
    assert enc.main([str(source), str(four), *common, "--jobs", "4"]) == 0
    assert one.read_bytes() == four.read_bytes()
    assert verify.verify(one)["ok"]
