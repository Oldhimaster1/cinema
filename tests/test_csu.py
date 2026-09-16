import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"

with tempfile.TemporaryDirectory() as directory:
    directory = pathlib.Path(directory)
    srt = directory / "a.srt"
    output = directory / "A.CSU"

    # Write the exact intended UTF-8 BOM + CRLF bytes. Path.write_text()
    # performs newline translation on Windows; passing literal CRLF to it can
    # create CRCRLF and silently turn every source line into a separate block.
    srt.write_bytes(
        b"\xef\xbb\xbf"
        b"1\r\n"
        b"00:00:00,000 --> 00:00:01,000\r\n"
        b"Hello <i>world</i>!\r\n"
        b"\r\n"
        b"2\r\n"
        b"00:00:02,000 --> 00:00:03,000\r\n"
        b"Second line\r\n"
    )
    subprocess.run(
        [
            sys.executable,
            str(TOOLS / "srt_to_csu.py"),
            str(srt),
            str(output),
            "--name", "MOVIE.BIN",
            "--frames", "100",
            "--fps-num", "18",
            "--movie-size", "999",
        ],
        check=True,
    )
    subprocess.run([sys.executable, str(TOOLS / "verify_csu.py"), str(output)], check=True)
    damaged = bytearray(output.read_bytes())
    damaged[-1] ^= 1
    output.write_bytes(damaged)
    result = subprocess.run([sys.executable, str(TOOLS / "verify_csu.py"), str(output)])
    assert result.returncode != 0

print("python CSU tests PASS")
