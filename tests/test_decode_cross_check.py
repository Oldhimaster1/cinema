"""Cross-checks the shipped calculator decoder (src/decode.c, compiled
with the host gcc into a shared library and called via ctypes -- the
actual C code that ships, not a reimplementation of it) against an
independent pure-Python reference model, on randomized frames. Requires
byte-identical output between the two.

decode.c only unpacks packed 4-bit pixels into a flat native-resolution
(no scaling) buffer -- see src/decode.h for why 2x scaling moved to
GraphX's own gfx_ScaledSprite_NoClip() instead of being done by hand
here."""
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
    lib.cinema_unpack_packed4.argtypes = [
        ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_uint8),
    ]
    lib.cinema_unpack_packed4.restype = None
    return lib


def c_decode(lib, packed: bytes) -> bytearray:
    # One guard byte past the real buffer so an overrun shows up as a
    # mismatch instead of silently corrupting unrelated memory.
    size = fmt.WIDTH * fmt.HEIGHT
    out = (ctypes.c_uint8 * (size + 1))(*([0xAA] * (size + 1)))
    packed_buf = (ctypes.c_uint8 * len(packed))(*packed)
    lib.cinema_unpack_packed4(packed_buf, out)
    return bytearray(out)


def python_reference_decode(packed: bytes) -> bytearray:
    """Independent reference model: not calling into decode.c, not
    calling into cin2_format's pack/unpack -- just the spec's own
    nibble-order rule (high nibble = even/first pixel), implemented
    separately. One extra guard byte to match c_decode's shape."""
    out = bytearray(fmt.WIDTH * fmt.HEIGHT + 1)
    out[-1] = 0xAA
    i = 0
    for byte in packed:
        out[i] = byte >> 4
        out[i + 1] = byte & 0x0F
        i += 2
    return out


@pytest.mark.parametrize("seed", range(20))
def test_random_frames_byte_identical(decode_lib, seed):
    rng = random.Random(seed)
    indices = [rng.randrange(16) for _ in range(fmt.WIDTH * fmt.HEIGHT)]
    packed = fmt.pack_frame(indices)

    c_result = c_decode(decode_lib, packed)
    py_result = python_reference_decode(packed)

    assert c_result == py_result, f"mismatch for seed={seed}"


def test_guard_byte_untouched(decode_lib):
    """The one byte past the real output buffer must never be written --
    confirms the unpack doesn't overrun its destination."""
    indices = [15] * (fmt.WIDTH * fmt.HEIGHT)  # maximal write pattern
    packed = fmt.pack_frame(indices)

    out = c_decode(decode_lib, packed)

    assert out[-1] == 0xAA, "wrote past the intended output size"


def test_reversed_nibble_order_is_high_then_low(decode_lib):
    """First byte 0x3C must decode to out[0]=3, out[1]=12 -- high nibble
    is the even (first) pixel, per docs/CIN2_FORMAT.md. This test would
    catch a nibble-order swap."""
    indices = [3, 12] + [0] * (fmt.WIDTH * fmt.HEIGHT - 2)
    packed = fmt.pack_frame(indices)

    out = c_decode(decode_lib, packed)

    assert out[0] == 3
    assert out[1] == 12


def test_last_pixel_edge_case(decode_lib):
    indices = [0] * (fmt.WIDTH * fmt.HEIGHT)
    indices[-1] = 9
    packed = fmt.pack_frame(indices)

    out = c_decode(decode_lib, packed)

    assert out[fmt.WIDTH * fmt.HEIGHT - 1] == 9
