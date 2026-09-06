#!/usr/bin/env python3
"""Independent CIN2 image verifier.

Deliberately does NOT import tools/cin2_format.py's parser -- this tool
re-implements header/CRC parsing itself so a bug shared between the
encoder and its own parser doesn't also hide in this check. (It does
reuse plain constants -- magic bytes, offsets, sizes -- from
docs/CIN2_FORMAT.md by hand, since those are the spec, not "the
encoder's logic".)

Usage:
    python3 tools/verify_cin2.py movie.bin [--json]

Exit status: 0 if the image is well-formed, 1 otherwise.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path
from typing import Any, Optional

MAGIC = b"CIN2"
VERSION = 2
HEADER_BYTES = 512
CRC_BYTES = 22
DATA_LBA = 1
FRAME_SECTORS = 30
SECTOR_BYTES = 512
PALETTE_ENTRIES = 16
WIDTH = 160
HEIGHT = 96
FRAME_BYTES = WIDTH * HEIGHT  # one byte/pixel (0..15), not bit-packed
# 30 sectors/frame * 512 bytes must equal one frame exactly -- this is a
# spec invariant, not something a header field asserts, so check it once
# here rather than trusting it.
assert FRAME_SECTORS * SECTOR_BYTES == FRAME_BYTES


def fail(errors: list, msg: str) -> None:
    errors.append(msg)


def verify(path: Path) -> dict:
    """Returns a result dict with keys: ok, errors, warnings, header
    (if parseable), file_size, expected_size, sha256."""
    errors: list = []
    warnings: list = []
    result: dict = {"file": str(path)}

    try:
        raw = path.read_bytes()
    except OSError as exc:
        return {"ok": False, "errors": [f"cannot read file: {exc}"], "warnings": []}

    result["file_size"] = len(raw)

    import hashlib
    result["sha256"] = hashlib.sha256(raw).hexdigest()

    if len(raw) < HEADER_BYTES:
        fail(errors, f"file shorter than header ({len(raw)} < {HEADER_BYTES} bytes)")
        return {"ok": False, "errors": errors, "warnings": warnings, **result}

    header_raw = raw[:HEADER_BYTES]

    # --- magic / version -------------------------------------------------
    magic = header_raw[0:4]
    if magic != MAGIC:
        fail(errors, f"bad magic: {magic!r} (expected {MAGIC!r})")
        return {"ok": False, "errors": errors, "warnings": warnings, **result}

    version = header_raw[4]
    if version != VERSION:
        fail(errors, f"unsupported version {version} (expected {VERSION})")
        return {"ok": False, "errors": errors, "warnings": warnings, **result}

    flags = header_raw[5]
    if flags != 0:
        fail(errors, f"unknown flags byte 0x{flags:02X} (must be 0)")

    # --- CRC ---------------------------------------------------------------
    stored_crc = struct.unpack_from("<L", header_raw, 22)[0]
    computed_crc = zlib.crc32(header_raw[:CRC_BYTES]) & 0xFFFFFFFF
    if stored_crc != computed_crc:
        fail(errors, f"bad header CRC: stored 0x{stored_crc:08X}, computed 0x{computed_crc:08X}")
        return {"ok": False, "errors": errors, "warnings": warnings, **result}

    # --- reserved bytes [58, 512) must be zero -----------------------------
    reserved = header_raw[58:HEADER_BYTES]
    if any(b != 0 for b in reserved):
        first_nonzero = next(i for i, b in enumerate(reserved) if b != 0)
        fail(errors, f"reserved byte nonzero at offset {58 + first_nonzero}")

    width, height, fps_num, fps_den, frame_count = struct.unpack_from("<HHLLL", header_raw, 6)
    palette = list(struct.unpack_from(f"<{PALETTE_ENTRIES}H", header_raw, 26))

    header_info = {
        "width": width, "height": height,
        "fps_num": fps_num, "fps_den": fps_den,
        "frame_count": frame_count,
        "palette": [f"0x{p:04X}" for p in palette],
    }
    result["header"] = header_info

    # --- dimensions ----------------------------------------------------
    if width == 0 or height == 0:
        fail(errors, f"zero dimension: {width}x{height}")
    elif width != WIDTH or height != HEIGHT:
        fail(errors, f"unsupported resolution {width}x{height} (only {WIDTH}x{HEIGHT} is decodable)")

    # --- fps -------------------------------------------------------------
    if fps_num == 0:
        fail(errors, "fps_num is zero")
    if fps_den == 0:
        fail(errors, "fps_den is zero (would divide by zero in the player's scheduler)")

    # --- frame_count / stream extent, overflow-checked -------------------
    # Use Python's arbitrary-precision ints (no wraparound) to compute
    # the true required extent, then separately confirm it also fits in
    # 32/64-bit ranges the way the C/ez80 code would compute it, so an
    # enormous frame_count can't be missed via silent overflow here either.
    if width == WIDTH and height == HEIGHT:
        bytes_per_frame = width * height
        if bytes_per_frame != FRAME_BYTES:
            fail(errors, f"computed bytes/frame {bytes_per_frame} != spec FRAME_BYTES {FRAME_BYTES}")

    required_sectors = DATA_LBA + frame_count * FRAME_SECTORS
    required_bytes = required_sectors * SECTOR_BYTES
    result["expected_size"] = required_bytes
    result["expected_frame_count"] = frame_count

    UINT32_MAX = 0xFFFFFFFF
    if required_sectors > UINT32_MAX:
        fail(errors, f"required_sectors {required_sectors} overflows uint32_t "
                      "(frame_count is implausibly large)")

    if frame_count == 0:
        fail(errors, "frame_count is zero (movie has no frames)")

    if len(raw) < required_bytes:
        fail(errors, f"file truncated: {len(raw)} bytes present, "
                      f"{required_bytes} required for {frame_count} frames "
                      f"(missing {required_bytes - len(raw)} bytes -- "
                      "final frame(s) incomplete)")
    elif len(raw) > required_bytes:
        warnings.append(f"{len(raw) - required_bytes} bytes of unexpected trailing "
                         "data after the last frame")

    # --- palette sanity (informational; any 16-bit value is technically
    # legal RGB1555 (the top bit is simply unused), so this can only warn,
    # not fail) --------------------------------------------------------
    if len(set(palette)) < PALETTE_ENTRIES:
        warnings.append(f"palette has only {len(set(palette))} distinct colors "
                         f"of {PALETTE_ENTRIES} entries")

    if fps_num and fps_den:
        result["expected_duration_s"] = frame_count * fps_den / fps_num

    ok = len(errors) == 0
    return {"ok": ok, "errors": errors, "warnings": warnings, **result}


def format_readable(result: dict) -> str:
    lines = [f"file: {result['file']}"]
    if "file_size" in result:
        lines.append(f"size: {result['file_size']} bytes")
    if "sha256" in result:
        lines.append(f"sha256: {result['sha256']}")
    if "header" in result:
        h = result["header"]
        lines.append(f"resolution: {h['width']}x{h['height']}")
        lines.append(f"fps: {h['fps_num']}/{h['fps_den']}")
        lines.append(f"frame_count: {h['frame_count']}")
        if "expected_duration_s" in result:
            lines.append(f"expected duration: {result['expected_duration_s']:.2f}s")
        if "expected_size" in result:
            lines.append(f"expected file size: {result['expected_size']} bytes")
    if result["warnings"]:
        lines.append("warnings:")
        lines += [f"  - {w}" for w in result["warnings"]]
    if result["errors"]:
        lines.append("errors:")
        lines += [f"  - {e}" for e in result["errors"]]
    lines.append("RESULT: " + ("PASS" if result["ok"] else "FAIL"))
    return "\n".join(lines)


def main(argv: Optional[list] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=Path)
    parser.add_argument("--json", action="store_true", help="emit JSON instead of readable text")
    args = parser.parse_args(argv)

    result = verify(args.image)

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(format_readable(result))

    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
