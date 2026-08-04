"""Cross-checks the shipped calculator decoder (src/decode.c, compiled
with the host gcc into a shared library and called via ctypes -- the
actual C code that ships, not a reimplementation of it) against an
independent pure-Python reference model, on randomized frames. Requires
byte-identical output between the two."""
import ctypes
import random
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import cin2_format as fmt  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent


@pytest.fixture(scope="module")
def decode_lib(tmp_path_factory):
    build_dir = tmp_path_factory.mktemp("decode_so")
    so_path = build_dir / "libdecode.so"
    subprocess.run(
        ["gcc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra",
         "-o", str(so_path), str(REPO_ROOT / "src" / "decode.c")],
        check=True,
    )
    lib = ctypes.CDLL(str(so_path))
    lib.cinema_draw_packed4_scaled2x.argtypes = [
        ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_uint16, ctypes.c_uint16,
    ]
    lib.cinema_draw_packed4_scaled2x.restype = None
    return lib


def c_decode(lib, packed: bytes, stride: int, y_offset: int, fb_height: int) -> bytearray:
    fb = (ctypes.c_uint8 * (fb_height * stride))(*([0xAA] * (fb_height * stride)))
    packed_buf = (ctypes.c_uint8 * len(packed))(*packed)
    lib.cinema_draw_packed4_scaled2x(packed_buf, fb, ctypes.c_uint16(stride),
                                      ctypes.c_uint16(y_offset))
    return bytearray(fb)


def python_reference_decode(packed: bytes, stride: int, y_offset: int, fb_height: int) -> bytearray:
    """Independent reference model: not calling into decode.c, not
    calling into cin2_format's pack/unpack -- just the spec's own
    nibble-order and 2x-nearest-neighbor rule, implemented separately."""
    fb = bytearray([0xAA] * (fb_height * stride))
    indices = []
    for byte in packed:
        indices.append(byte >> 4)
        indices.append(byte & 0x0F)

    for y in range(fmt.HEIGHT):
        for x in range(fmt.WIDTH):
            value = indices[y * fmt.WIDTH + x]
            for dy in (0, 1):
                for dx in (0, 1):
                    row = y_offset + y * 2 + dy
                    col = x * 2 + dx
                    fb[row * stride + col] = value
    return fb


@pytest.mark.parametrize("seed", range(20))
def test_random_frames_byte_identical(decode_lib, seed):
    rng = random.Random(seed)
    indices = [rng.randrange(16) for _ in range(fmt.WIDTH * fmt.HEIGHT)]
    packed = fmt.pack_frame(indices)

    stride = 320
    y_offset = 24
    fb_height = 240

    c_result = c_decode(decode_lib, packed, stride, y_offset, fb_height)
    py_result = python_reference_decode(packed, stride, y_offset, fb_height)

    assert c_result == py_result, f"mismatch for seed={seed}"


def test_guard_regions_untouched(decode_lib):
    """Bytes outside [y_offset, y_offset + 192) rows must never be
    written -- confirms the blit doesn't overrun its destination
    region."""
    indices = [15] * (fmt.WIDTH * fmt.HEIGHT)  # maximal write pattern
    packed = fmt.pack_frame(indices)
    stride = 320
    y_offset = 24
    fb_height = 240

    fb = c_decode(decode_lib, packed, stride, y_offset, fb_height)

    before = fb[: y_offset * stride]
    after = fb[(y_offset + fmt.HEIGHT * 2) * stride:]
    assert all(b == 0xAA for b in before), "wrote above the intended y_offset"
    assert all(b == 0xAA for b in after), "wrote below the intended frame area"


def test_reversed_nibble_order_is_high_then_low(decode_lib):
    """First byte 0x3C must decode to left=3, right=12 -- high nibble is
    the even (left) pixel, per docs/CIN2_FORMAT.md. This test would
    catch a nibble-order swap."""
    indices = [3, 12] + [0] * (fmt.WIDTH * fmt.HEIGHT - 2)
    packed = fmt.pack_frame(indices)
    stride = 320
    y_offset = 0

    fb = c_decode(decode_lib, packed, stride, y_offset, 96 * 2)

    assert fb[0] == 3
    assert fb[1] == 3
    assert fb[2] == 12
    assert fb[3] == 12


def test_last_pixel_edge_case(decode_lib):
    indices = [0] * (fmt.WIDTH * fmt.HEIGHT)
    indices[-1] = 9
    packed = fmt.pack_frame(indices)
    stride = 320
    y_offset = 0
    fb_height = fmt.HEIGHT * 2

    fb = c_decode(decode_lib, packed, stride, y_offset, fb_height)

    last_row0 = (fb_height - 2) * stride
    last_row1 = (fb_height - 1) * stride
    assert fb[last_row0 + stride - 1] == 9
    assert fb[last_row0 + stride - 2] == 9
    assert fb[last_row1 + stride - 1] == 9
    assert fb[last_row1 + stride - 2] == 9
