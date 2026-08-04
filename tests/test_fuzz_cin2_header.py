"""Fuzzes tools/verify_cin2.py's header parser with randomized and
mutated headers. Requirement: it must never crash (unhandled exception),
never hang, and must always return a clean ok/errors verdict -- i.e. it
either accepts a well-formed header or explains why it rejected one, and
nothing else. This is a parser-safety property, independent of whether
any given random header happens to be "valid"."""
import random
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import verify_cin2  # noqa: E402
import cin2_format as fmt  # noqa: E402

FUZZ_ITERATIONS = 3000
PER_CALL_TIMEOUT_S = 1.0


def _well_formed_raw() -> bytes:
    header = fmt.Cin2Header(width=fmt.WIDTH, height=fmt.HEIGHT, fps_num=24, fps_den=1,
                             frame_count=4, palette=[0] * 16)
    raw = bytearray(fmt.build_header(header))
    for f in range(4):
        raw += fmt.pack_frame([0] * (fmt.WIDTH * fmt.HEIGHT))
    return bytes(raw)


def test_fully_random_headers_never_crash(tmp_path):
    rng = random.Random(1234)
    for i in range(FUZZ_ITERATIONS):
        raw = bytes(rng.getrandbits(8) for _ in range(rng.choice([0, 1, 10, 511, 512, 513, 8192])))
        path = tmp_path / f"fuzz_{i}.bin"
        path.write_bytes(raw)

        start = time.monotonic()
        try:
            result = verify_cin2.verify(path)
        except Exception as exc:  # noqa: BLE001 -- the whole point is "never raises"
            raise AssertionError(f"verify() raised on random input #{i}: {exc!r}") from exc
        elapsed = time.monotonic() - start

        assert elapsed < PER_CALL_TIMEOUT_S, f"verify() took {elapsed:.3f}s on input #{i} (hang?)"
        assert "ok" in result and isinstance(result["ok"], bool)
        assert isinstance(result["errors"], list)
        if not result["ok"]:
            assert result["errors"], f"input #{i}: rejected with no error message"


def test_mutated_well_formed_headers_never_crash(tmp_path):
    """Bit/byte flips of an otherwise-valid header -- more likely than
    pure noise to land on "nearly valid but subtly wrong" inputs, which
    is where off-by-one and overflow bugs tend to hide."""
    base = bytearray(_well_formed_raw())
    rng = random.Random(5678)

    for i in range(FUZZ_ITERATIONS):
        mutant = bytearray(base)
        num_mutations = rng.randint(1, 8)
        for _ in range(num_mutations):
            pos = rng.randrange(len(mutant))
            mutant[pos] = rng.getrandbits(8)

        path = tmp_path / f"mutant_{i}.bin"
        path.write_bytes(bytes(mutant))

        try:
            result = verify_cin2.verify(path)
        except Exception as exc:  # noqa: BLE001
            raise AssertionError(f"verify() raised on mutant #{i}: {exc!r}") from exc

        if not result["ok"]:
            assert result["errors"], f"mutant #{i}: rejected with no error message"


def test_specifically_crafted_overflow_headers():
    """Header fields set to extreme values designed to overflow naive
    32-bit multiplication (frame_count * FRAME_SECTORS)."""
    good = bytearray(_well_formed_raw())

    for frame_count in (0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x100000, 2**32 - 1):
        raw = bytearray(good[: fmt.HEADER_BYTES])
        struct.pack_into("<L", raw, 18, frame_count & 0xFFFFFFFF)
        struct.pack_into("<L", raw, 22, zlib.crc32(bytes(raw[:22])) & 0xFFFFFFFF)

        # verify_cin2.verify() takes a path; write one instead of adding
        # a test-only API to the shipped tool.
        import tempfile
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(bytes(raw))
            tmp_path = Path(f.name)
        try:
            result = verify_cin2.verify(tmp_path)
        finally:
            tmp_path.unlink()

        assert not result["ok"], f"frame_count={frame_count:#x} should be rejected"
        assert result["errors"]
