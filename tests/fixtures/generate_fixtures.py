#!/usr/bin/env python3
"""Generates the canonical CIN2 conformance fixtures under tests/fixtures/
and tests/fixtures/MANIFEST.json. Deterministic: run twice, get
byte-identical output (checked by tests/test_fixtures.py).

Run this only when you intend to change the fixture set -- the manifest
is committed and treated as golden; tests compare against it rather
than regenerating it on failure (a failing test is a real regression,
not something to "fix" by re-running this script).
"""
from __future__ import annotations

import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "tools"))
import cin2_format as fmt  # noqa: E402

FIXTURES_DIR = Path(__file__).resolve().parent
manifest: dict = {"good": {}, "bad": {}}


def solid_frame(index: int) -> list[int]:
    return [index] * (fmt.WIDTH * fmt.HEIGHT)


def write_good(name: str, raw: bytes, expected_ok: bool, note: str) -> None:
    path = FIXTURES_DIR / f"{name}.bin"
    path.write_bytes(raw)
    manifest["good"][name] = {
        "file": path.name,
        "size": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "expected_verifier_ok": expected_ok,
        "note": note,
    }


def write_bad(name: str, raw: bytes, note: str) -> None:
    path = FIXTURES_DIR / f"{name}.bin"
    path.write_bytes(raw)
    manifest["bad"][name] = {
        "file": path.name,
        "size": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "note": note,
    }


def build_movie(frame_count: int, fps_num: int, fps_den: int, palette,
                 frame_fn) -> bytes:
    header = fmt.Cin2Header(width=fmt.WIDTH, height=fmt.HEIGHT, fps_num=fps_num,
                             fps_den=fps_den, frame_count=frame_count, palette=palette)
    out = bytearray(fmt.build_header(header))
    for f in range(frame_count):
        out += fmt.pack_frame(frame_fn(f))
    return bytes(out)


DEFAULT_PALETTE = [
    0x0000, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF, 0xFFFF,
    0x8410, 0xC618, 0x4208, 0x2104, 0x8000, 0x0400, 0x0010, 0x7BEF,
]


def main() -> None:
    # --- good fixtures ---------------------------------------------------
    write_good("all_black", build_movie(1, 24, 1, DEFAULT_PALETTE, lambda f: solid_frame(0)),
               True, "single frame, every pixel index 0")
    write_good("all_max_index", build_movie(1, 24, 1, DEFAULT_PALETTE, lambda f: solid_frame(15)),
               True, "single frame, every pixel index 15 (0xFF packed bytes)")

    def alternating_nibbles(_f):
        return [(x % 2) * 15 for _y in range(fmt.HEIGHT) for x in range(fmt.WIDTH)]
    write_good("alternating_nibbles",
               build_movie(1, 24, 1, DEFAULT_PALETTE, alternating_nibbles),
               True, "index alternates 0,15,0,15... each row -> packed bytes are all 0x0F")

    def color_bars(_f):
        bar_width = fmt.WIDTH // 16
        return [min(x // bar_width, 15) for _y in range(fmt.HEIGHT) for x in range(fmt.WIDTH)]
    write_good("color_bars_16", build_movie(1, 24, 1, DEFAULT_PALETTE, color_bars),
               True, "16 vertical bars, one per palette entry")

    def unique_row_pattern(_f):
        return [y % 16 for y in range(fmt.HEIGHT) for _x in range(fmt.WIDTH)]
    write_good("unique_pattern_per_row",
               build_movie(1, 24, 1, DEFAULT_PALETTE, unique_row_pattern),
               True, "row y is solid index (y % 16); detects row-stride/vertical placement bugs")

    def bottom_right_only(_f):
        idx = [0] * (fmt.WIDTH * fmt.HEIGHT)
        idx[-1] = 15
        return idx
    write_good("bottom_right_pixel_only",
               build_movie(1, 24, 1, DEFAULT_PALETTE, bottom_right_only),
               True, "only the last logical pixel (bottom-right) is nonzero; "
                     "detects last-pixel/off-by-one edge cases")

    write_good("one_frame_movie",
               build_movie(1, 24, 1, DEFAULT_PALETTE, lambda f: solid_frame(1)),
               True, "exactly 1 frame -- smallest legal movie")
    write_good("four_frame_movie",
               build_movie(4, 24, 1, DEFAULT_PALETTE, lambda f: solid_frame(f)),
               True, "exactly 4 frames -- matches the player's slot count exactly")
    write_good("five_frame_movie",
               build_movie(5, 24, 1, DEFAULT_PALETTE, lambda f: solid_frame(f)),
               True, "exactly 5 frames -- first slot-queue refill after prefill")

    def frame_number_marker(f):
        # Top-left CINEMA_V2_WIDTH/16-wide block encodes (f % 16) as a
        # solid index -- a synthetic stand-in for a rendered frame
        # number (no font rendering in this repo), used by
        # tests/test_fixtures.py and manual/decode_cin2.py inspection to
        # confirm frames decode in the right order.
        idx = [(f * 3) % 16] * (fmt.WIDTH * fmt.HEIGHT)
        marker = f % 16
        block_w = fmt.WIDTH // 16
        for y in range(fmt.HEIGHT // 4):
            for x in range(block_w):
                idx[y * fmt.WIDTH + x] = marker
        return idx

    write_good("short_24_1", build_movie(24, 24, 1, DEFAULT_PALETTE, frame_number_marker),
               True, "24 frames @ 24/1 = exactly 1.0s; frame-number marker block top-left")
    write_good("short_24000_1001",
               build_movie(24, 24000, 1001, DEFAULT_PALETTE, frame_number_marker),
               True, "24 frames @ 24000/1001 = 1.001s; distinct duration from short_24_1")
    write_good("resume_fixture",
               build_movie(30, 24, 1, DEFAULT_PALETTE, frame_number_marker),
               True, "30 frames (1.25s @ 24fps) with a frame-number marker block, "
                     "for resume-position tests")

    # --- bad fixtures ------------------------------------------------------
    good_raw = build_movie(4, 24, 1, DEFAULT_PALETTE, lambda f: solid_frame(f))

    b = bytearray(good_raw)
    b[0:4] = b"CIN1"
    write_bad("bad_magic", bytes(b), "magic changed from CIN2 to CIN1")

    b = bytearray(good_raw)
    b[4] = 99
    write_bad("unsupported_version", bytes(b), "version byte set to 99")

    b = bytearray(good_raw)
    b[22] ^= 0xFF
    write_bad("bad_header_crc", bytes(b), "one CRC byte flipped")

    header = fmt.Cin2Header(width=0, height=fmt.HEIGHT, fps_num=24, fps_den=1,
                             frame_count=4, palette=DEFAULT_PALETTE)
    raw = bytearray(fmt.build_header(header))
    for f in range(4):
        raw += fmt.pack_frame(solid_frame(f))
    write_bad("zero_width_good_crc", bytes(raw),
              "width=0 with a correct CRC over the corrupted header -- "
              "exercises the dimension check specifically, not the CRC check")

    header = fmt.Cin2Header(width=fmt.WIDTH, height=0, fps_num=24, fps_den=1,
                             frame_count=4, palette=DEFAULT_PALETTE)
    raw = bytearray(fmt.build_header(header))
    for f in range(4):
        raw += fmt.pack_frame(solid_frame(f))
    write_bad("zero_height_good_crc", bytes(raw), "height=0 with a correct CRC")

    header = fmt.Cin2Header(width=320, height=240, fps_num=24, fps_den=1,
                             frame_count=1, palette=DEFAULT_PALETTE)
    raw = bytearray(fmt.build_header(header))
    write_bad("wrong_dimensions_good_crc", bytes(raw),
              "320x240 (a real but unsupported resolution) with a correct CRC, no frame data")

    header = fmt.Cin2Header(width=fmt.WIDTH, height=fmt.HEIGHT, fps_num=0, fps_den=1,
                             frame_count=4, palette=DEFAULT_PALETTE)
    raw = bytearray(fmt.build_header(header))
    for f in range(4):
        raw += fmt.pack_frame(solid_frame(f))
    write_bad("zero_fps_num", bytes(raw), "fps_num=0 with a correct CRC")

    header = fmt.Cin2Header(width=fmt.WIDTH, height=fmt.HEIGHT, fps_num=24, fps_den=0,
                             frame_count=4, palette=DEFAULT_PALETTE)
    raw = bytearray(fmt.build_header(header))
    for f in range(4):
        raw += fmt.pack_frame(solid_frame(f))
    write_bad("zero_fps_den", bytes(raw), "fps_den=0 (would divide by zero in the scheduler)")

    header = fmt.Cin2Header(width=fmt.WIDTH, height=fmt.HEIGHT, fps_num=24, fps_den=1,
                             frame_count=0, palette=DEFAULT_PALETTE)
    raw = bytearray(fmt.build_header(header))
    write_bad("zero_frame_count", bytes(raw), "frame_count=0, no frame data, correct CRC")

    header = fmt.Cin2Header(width=fmt.WIDTH, height=fmt.HEIGHT, fps_num=24, fps_den=1,
                             frame_count=0xFFFFFFFF, palette=DEFAULT_PALETTE)
    raw = bytearray(fmt.build_header(header))
    write_bad("huge_frame_count", bytes(raw),
              "frame_count=0xFFFFFFFF, correct CRC, no frame data -- "
              "required stream extent vastly exceeds the file / any real drive")

    b = bytearray(good_raw)
    b[5] = 0x01  # flags byte
    # CRC does not cover the flags byte's *value* change meaning here --
    # wait: CRC_BYTES covers offset [0,22) which INCLUDES offset 5, so
    # changing it invalidates the CRC too. That's fine: this fixture
    # exercises the "corrupted header rejected" path via the flags byte
    # specifically, still expected to fail (on CRC, same end result).
    write_bad("unknown_flags", bytes(b), "flags byte set to 0x01 (also invalidates the CRC, "
              "since flags is CRC-covered -- rejection is still due to the flags byte changing)")

    b = bytearray(good_raw)
    b[100] = 0xAA  # inside reserved [58, 512)
    write_bad("nonzero_reserved", bytes(b), "one reserved byte (offset 100) set nonzero")

    write_bad("truncated_header", good_raw[:100], "only 100 of 512 header bytes present")

    write_bad("truncated_frame", good_raw[: fmt.HEADER_BYTES + fmt.PACKED_BYTES // 2],
              "header plus half of frame 0's data; frames 1-3 and the rest of frame 0 missing")

    write_bad("trailing_data", good_raw + b"\x00" * 37,
              "37 stray bytes appended after the last frame (verifier: warning, not failure, "
              "on its own -- listed under bad/ here only because it's a malformed-media probe, "
              "see tests/test_fixtures.py for the exact expected outcome)")

    # --- synthetic v1 fixture ------------------------------------------
    # v1 has no header at all -- LBA 0 is frame 0's palette directly.
    # This is a synthetic stand-in (this repo has no v1 encoder; the
    # real one, FBin, is external) built only to exercise v1
    # detection/playback/completion-sync logic, with deliberately
    # radically different palettes on adjacent frames.
    v1_frames = 3
    v1_raw = bytearray()
    for f in range(v1_frames):
        palette565 = [((f + i) * 0x2A21) & 0xFFFF for i in range(256)]
        for p in palette565:
            v1_raw += struct.pack("<H", p)
        image = bytes([(f * 85 + x) & 0xFF for x in range(160 * 96)])
        v1_raw += image
    write_bad("v1_synthetic", bytes(v1_raw),
              "NOT malformed -- a synthetic v1 (legacy) image: 3 frames, 31 sectors each, "
              "radically different palettes per frame, no CIN2 magic. Filed under bad/ only "
              "because it's a non-CIN2 fixture the verifier correctly refuses to parse as v2; "
              "see PHASE 9 / v1 regression notes in RESULTS.md for how it's actually used.")

    manifest_path = FIXTURES_DIR / "MANIFEST.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"wrote {len(manifest['good'])} good + {len(manifest['bad'])} bad fixtures, "
          f"manifest at {manifest_path}")


if __name__ == "__main__":
    main()
