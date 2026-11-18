"""GUI logic can be validated without a desktop display or tkinter installed."""
import sys
import os
import signal
import shutil
import subprocess
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cinema_gui import build_encode_command  # noqa: E402


def test_gui_command_preserves_exact_paths_and_safe_switches():
    video = '/tmp/movie with spaces;$(touch danger).mp4'
    output = '/tmp/a surprising output.bin'
    cmd = build_encode_command(video, output, fps="24000/1001", fit="cover",
                               dither="stable", palette="weighted", packed4=True,
                               subtitles="/tmp/subtitles track.srt", jobs=3)
    assert cmd[2:4] == [video, output]
    assert cmd[cmd.index("--fps") + 1] == "24000/1001"
    assert "--packed4" in cmd
    assert cmd[cmd.index("--subtitles") + 1] == "/tmp/subtitles track.srt"
    assert cmd[cmd.index("--jobs") + 1] == "3"


@pytest.mark.parametrize("kwargs", [
    {"fps": "0"}, {"fps": "-5"}, {"fps": "100"},
    {"fps": "invalid"}, {"fps": "10/0"},
    {"fit": "destroy"}, {"dither": "random"},
    {"palette": "neon"}, {"jobs": -1}, {"jobs": 999},
])
def test_gui_rejects_invalid_settings(kwargs):
    with pytest.raises((ValueError, ZeroDivisionError)):
        build_encode_command("input.mp4", "output.bin", **kwargs)


def test_gui_rejects_batch_with_subtitle():
    with pytest.raises(ValueError):
        build_encode_command("/tmp", "output_dir", subtitles="subs.srt")


@pytest.mark.skipif(os.name == "nt" or shutil.which("ffmpeg") is None,
                    reason="POSIX SIGINT cleanup and FFmpeg required")
def test_cancelled_converter_removes_partial_output(tmp_path):
    """A GUI cancellation must not strand a giant half-encoded movie file."""
    video = tmp_path / "long.mp4"
    output = tmp_path / "movie.bin"
    subprocess.run([
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "testsrc2=duration=30:size=320x180:rate=30",
        "-c:v", "mpeg4", str(video),
    ], check=True)
    command = build_encode_command(str(video), str(output),
                                   fps="30", jobs=1)
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    partial = output.with_name(output.name + ".partial")
    try:
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if partial.exists() and partial.stat().st_size > 512:
                break
            if process.poll() is not None:
                pytest.fail("converter exited before cancellation exercise")
            time.sleep(0.05)
        else:
            pytest.fail("converter never reached active encode stage")
        process.send_signal(signal.SIGINT)
        process.communicate(timeout=18)
        assert process.returncode != 0
        assert not output.exists()
        assert not partial.exists()
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=5)
