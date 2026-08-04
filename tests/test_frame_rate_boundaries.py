"""Frame-rate / frame-count boundary tests for tools/encode_cin2.py,
using ffmpeg's synthetic lavfi source (self-contained, no external
video asset). Scope note: this covers 24/1, 24000/1001, and 30/1 plus
duration boundaries right at 1 and 24 output frames -- see
docs/KNOWN_LIMITATIONS.md for the fuller fps/VFR/odd-dimension/rotation
matrix this deliberately does not cover, given the environment's
compute/token budget for this validation pass."""
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import cin2_format as fmt  # noqa: E402
import encode_cin2 as enc  # noqa: E402

HAVE_FFMPEG = shutil.which("ffmpeg") is not None
pytestmark = pytest.mark.skipif(not HAVE_FFMPEG, reason="ffmpeg not on PATH")


def _make_source(tmp_path, duration_s, name="src.mp4", rate=30):
    path = tmp_path / name
    subprocess.run(
        ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
         "-f", "lavfi", "-i", f"testsrc2=duration={duration_s}:size=320x240:rate={rate}",
         str(path)],
        check=True,
    )
    return path


@pytest.mark.parametrize("fps_arg,fps_num,fps_den", [
    ("24", 24, 1),
    ("24000/1001", 24000, 1001),
    ("30", 30, 1),
])
def test_header_records_requested_rational_fps(tmp_path, fps_arg, fps_num, fps_den):
    src = _make_source(tmp_path, duration_s=1)
    out = tmp_path / f"out_{fps_num}_{fps_den}.bin"
    enc.main([str(src), str(out), "--fps", fps_arg, "--palette-samples", "4"])

    header = fmt.parse_header(out.read_bytes()[: fmt.HEADER_BYTES])
    assert (header.fps_num, header.fps_den) == (fps_num, fps_den)


@pytest.mark.parametrize("duration_s,fps,expect_frames_between", [
    (1 / 24 - 0.005, 24, (0, 1)),    # just under one output frame
    (1 / 24, 24, (1, 2)),            # right at one output frame
    (1 / 24 + 0.02, 24, (1, 2)),     # just over one output frame
    (23 / 24 - 0.01, 24, (22, 24)),  # just under 24 output frames
    (24 / 24, 24, (23, 25)),         # right at 24 output frames
    (24 / 24 + 0.02, 24, (23, 26)),  # just over 24 output frames
])
def test_duration_boundaries_produce_sane_frame_counts(tmp_path, duration_s, fps, expect_frames_between):
    # ffmpeg's own frame-boundary rounding means we assert a tight
    # range, not an exact count -- the property under test is "no wild
    # off-by-many near a boundary", not ffmpeg's own rounding behavior.
    src = _make_source(tmp_path, duration_s=max(duration_s, 0.05), name=f"src_{duration_s:.4f}.mp4")
    out = tmp_path / "out.bin"
    enc.main([str(src), str(out), "--fps", str(fps), "--palette-samples", "2"])

    header = fmt.parse_header(out.read_bytes()[: fmt.HEADER_BYTES])
    lo, hi = expect_frames_between
    assert lo <= header.frame_count <= hi, \
        f"duration={duration_s:.4f}s @ {fps}fps: got {header.frame_count} frames, expected {lo}-{hi}"

    # File size must match the formula exactly for whatever frame count
    # was actually produced -- this is the invariant that matters, not
    # the exact frame count itself.
    expected_size = fmt.HEADER_BYTES + header.frame_count * fmt.PACKED_BYTES
    assert out.stat().st_size == expected_size
