"""Real FFmpeg round-trips for aspect-preserving Cinema conversion."""
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import encode_cin2 as enc  # noqa: E402
import cin2_format as fmt  # noqa: E402
import decode_cin2 as dec  # noqa: E402
import verify_cin2 as verify  # noqa: E402

pytestmark = pytest.mark.skipif(not shutil.which("ffmpeg"), reason="FFmpeg required")


def _wide_source(tmp_path):
    source = tmp_path / "wide.mp4"
    subprocess.run([
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "color=c=red:size=320x180:rate=15:duration=1",
        "-c:v", "mpeg4", str(source),
    ], check=True)
    return source


def test_contain_letterboxes_without_stretching_or_changing_frame_size(tmp_path):
    source = _wide_source(tmp_path)
    results = {}
    for fit in ("stretch", "contain", "cover"):
        # Consume the whole pipe so FFmpeg exits cleanly; abandoning the
        # generator after one frame would break its stdout pipe.
        frame = list(enc.stream_raw_frames(source, 15, 1, None, 1.0,
                                           fit_mode=fit))[0]
        assert frame.size == (160, 96)
        results[fit] = frame
    assert results["stretch"].getpixel((0, 0))[0] > 200
    assert results["contain"].getpixel((0, 0)) == (0, 0, 0)
    assert results["contain"].getpixel((80, 48))[0] > 200
    assert results["cover"].getpixel((0, 0))[0] > 200


@pytest.mark.parametrize("packed", [False, True])
def test_contain_output_is_valid_and_has_black_bars(tmp_path, packed):
    source = _wide_source(tmp_path)
    movie = tmp_path / "WIDE.BIN"
    args = [str(source), str(movie), "--fps", "15", "--jobs", "1",
            "--palette-samples", "8", "--fit", "contain"]
    if packed:
        args.append("--packed4")
    assert enc.main(args) == 0
    assert verify.verify(movie)["ok"]
    raw, header = dec.read_movie(movie)
    try:
        indices = dec.frame_indices(raw, header, 0)
        palette = [enc.rgb1555_to_rgb888(v) for v in header.palette]
        top = palette[indices[0]]
        middle = palette[indices[48 * 160 + 80]]
        assert max(top) < 32
        assert middle[0] > middle[1] + 80
        assert middle[0] > middle[2] + 80
    finally:
        raw.close()


def test_invalid_fit_mode_is_rejected(tmp_path):
    with pytest.raises(ValueError, match="unknown fit mode"):
        list(enc.stream_raw_frames(tmp_path / "anything.mp4", 15, 1,
                                   None, None, fit_mode="diagonal"))
