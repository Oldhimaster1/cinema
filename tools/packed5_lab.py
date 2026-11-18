#!/usr/bin/env python3
"""EXPERIMENT ONLY: 32-color, 5-bits-per-pixel design study for Cinema.

This produces no CIN2 output. The released CIN2 v2 format has exactly 16
palette entries and Packed4 is 4bpp; writing a 32-color movie with its
current header would silently make invalid media. Use this to compare
quality, frame footprints, and candidate bit-packing before a format v3.
"""
from __future__ import annotations

import argparse
from pathlib import Path
from typing import Iterable, Sequence

from PIL import Image

from encode_cin2 import (HEIGHT, WIDTH, build_global_palette,
                         palette_image_to_rgb1555, rgb1555_to_rgb888,
                         rgb888_to_rgb1555, stream_raw_frames)

FRAME_PIXELS = WIDTH * HEIGHT
PACKED5_BYTES = FRAME_PIXELS * 5 // 8  # 9,600 bytes
PACKED5_SECTORS = (PACKED5_BYTES + 511) // 512  # 19 sectors / 9,728 bytes


def pack5(indices: Sequence[int]) -> bytes:
    """Pack exactly one 160x96 frame, most-significant 5 bits first."""
    if len(indices) != FRAME_PIXELS:
        raise ValueError(f"requires {FRAME_PIXELS} pixels")
    result = bytearray(PACKED5_BYTES)
    accumulator = 0
    bits = 0
    dest = 0
    for index in indices:
        if not 0 <= index < 32:
            raise ValueError("palette index must be 0..31")
        accumulator = (accumulator << 5) | index
        bits += 5
        if bits >= 8:
            bits -= 8
            result[dest] = (accumulator >> bits) & 255
            dest += 1
            accumulator &= (1 << bits) - 1
    assert bits == 0 and dest == PACKED5_BYTES
    return bytes(result)


def unpack5(data: bytes) -> bytes:
    """Decode a packed frame to one byte/pixel for semantic comparison."""
    if len(data) != PACKED5_BYTES:
        raise ValueError(f"requires {PACKED5_BYTES} bytes")
    result = bytearray(FRAME_PIXELS)
    accumulator = bits = dest = 0
    for value in data:
        accumulator = (accumulator << 8) | value
        bits += 8
        while bits >= 5:
            bits -= 5
            result[dest] = (accumulator >> bits) & 31
            dest += 1
            accumulator &= (1 << bits) - 1
    assert bits == 0 and dest == FRAME_PIXELS
    return bytes(result)


def palette32(frames: Sequence[Image.Image]) -> Image.Image:
    """Build 32 LCD-precision entries, sampled across representative frames."""
    if not frames:
        raise ValueError("at least one frame required")
    cols = min(len(frames), 8)
    rows = (len(frames) + cols - 1) // cols
    sheet = Image.new("RGB", (WIDTH * cols, HEIGHT * rows))
    for i, frame in enumerate(frames):
        sheet.paste(frame.convert("RGB"), ((i % cols) * WIDTH, (i // cols) * HEIGHT))
    quantized = sheet.quantize(colors=32, method=Image.Quantize.MEDIANCUT,
                               dither=Image.Dither.NONE)
    values = quantized.getpalette() or []
    palette_values = []
    for i in range(32):
        at = 3 * i
        palette_values.append(rgb888_to_rgb1555(*values[at:at + 3]))
    raw = []
    for val in palette_values:
        raw.extend(rgb1555_to_rgb888(val))
    # Pillow considers *all* 256 slots of a P-mode palette, even when
    # they were only padding. Repeat index zero so no pad slot is closer
    # to source black than all legitimate 32 entries.
    raw.extend(raw[:3] * (256 - 32))
    palette = Image.new("P", (1, 1))
    palette.putpalette(raw)
    return palette


def quality_metrics(frames: Iterable[Image.Image], palette: Image.Image,
                    count: int) -> tuple[int, int]:
    """Return (green-weighted squared RGB error, pixels), actual LCD colors."""
    colors = [tuple((palette.getpalette() or [])[i * 3:i * 3 + 3])
              for i in range(count)]
    total = pixels = 0
    for frame in frames:
        rgb = frame.convert("RGB")
        indices = rgb.quantize(palette=palette, dither=Image.Dither.NONE).tobytes()
        for (r, g, b), idx in zip(rgb.get_flattened_data(), indices):
            # Mirror the v2 encoder's 16-color index clamp when testing
            # its real output, including padded-Palette edge cases.
            cr, cg, cb = colors[min(idx, count - 1)]
            total += 2 * (r - cr) ** 2 + 4 * (g - cg) ** 2 + (b - cb) ** 2
            pixels += 1
    return total, pixels


def compare(frames: Sequence[Image.Image]) -> dict:
    base16 = build_global_palette(frames, len(frames))
    candidate32 = palette32(frames)
    e16, n16 = quality_metrics(frames, base16, 16)
    e32, n32 = quality_metrics(frames, candidate32, 32)
    assert n16 == n32
    # Independently prove a real 32-color candidate can be serialized.
    for frame in frames:
        indices = frame.convert("RGB").quantize(
            palette=candidate32, dither=Image.Dither.NONE).tobytes()
        assert unpack5(pack5(indices)) == indices
    return {"pixels": n16, "weighted_mse_16": e16 / n16,
            "weighted_mse_32": e32 / n32,
            "improvement_percent": 100 * (1 - e32 / e16) if e16 else 0,
            "packed4_bytes_per_frame": 7680, "packed5_bytes_per_frame": PACKED5_BYTES,
            "packed5_sector_bytes": PACKED5_SECTORS * 512,
            "packed5_kib_per_second_24fps": PACKED5_SECTORS * 512 * 24 / 1024,
            "packed5_kib_per_second_30fps": PACKED5_SECTORS * 512 * 30 / 1024}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("video", type=Path)
    parser.add_argument("--frames", type=int, default=16)
    args = parser.parse_args()
    if not 1 <= args.frames <= 128:
        parser.error("--frames must be 1..128")
    frames = list(stream_raw_frames(args.video, 5, 1, None, None,
                                    max_frames=args.frames, fit_mode="contain"))
    if not frames:
        parser.error("no frames extracted")
    for key, value in compare(frames).items():
        print(f"{key}: {value}")
    print("EXPERIMENT ONLY: not a valid or playable CIN2 v2 format")


if __name__ == "__main__":
    main()
