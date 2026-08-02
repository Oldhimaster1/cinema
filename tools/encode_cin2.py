#!/usr/bin/env python3
"""Encodes a video into the Cinema v2 (CIN2) format for the TI-84 Plus CE.

See docs/CIN2_FORMAT.md for the on-disk format this produces: a 512-byte
header (magic, resolution, frame rate, frame count, one shared 16-color
palette) followed by 160x96 packed-4-bit frames, 15 sectors each.

Usage:

    python3 tools/encode_cin2.py input.mp4 output.bin \\
        [--fps 24] [--palette-samples 32] [--start 0] [--duration 60]

Then write output.bin to a USB drive starting at LBA 0/byte 0, e.g.:

    sudo dd if=output.bin of=/dev/sdX bs=1M conv=fsync

(Linux/macOS) or with HDD Raw Copy Tool on Windows. Requires ffmpeg on
PATH to decode the input video, and Pillow for palette quantization.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional, Sequence, Tuple

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cin2_format as fmt  # noqa: E402


def rgb888_to_rgb565(r: int, g: int, b: int) -> int:
    """Packs 8-bit RGB into the RGB565 layout gfx_SetPalette expects."""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def extract_frames(video_path: Path, out_dir: Path, fps_num: int, fps_den: int,
                    start: Optional[float], duration: Optional[float]) -> list[Path]:
    """Runs ffmpeg to decode video_path into WIDTHxHEIGHT RGB PNG frames
    at fps_num/fps_den into out_dir, returning the sorted frame paths."""
    if shutil.which("ffmpeg") is None:
        raise RuntimeError("ffmpeg not found on PATH -- required to decode the input video")

    cmd = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error"]
    if start is not None:
        cmd += ["-ss", str(start)]
    cmd += ["-i", str(video_path)]
    if duration is not None:
        cmd += ["-t", str(duration)]
    cmd += [
        "-vf", f"fps={fps_num}/{fps_den},scale={fmt.WIDTH}:{fmt.HEIGHT}:flags=lanczos",
        "-pix_fmt", "rgb24",
        str(out_dir / "frame_%08d.png"),
    ]
    subprocess.run(cmd, check=True)

    frames = sorted(out_dir.glob("frame_*.png"))
    if not frames:
        raise RuntimeError("ffmpeg produced no frames -- check the input video/time range")
    return frames


def build_global_palette(frames: Sequence[Image.Image], sample_count: int) -> Image.Image:
    """Builds one 16-color adaptive palette representative of the whole
    movie by tiling a sample of frames into a single sheet and
    quantizing that. Returns a "P"-mode Image whose palette is the
    result; pass it to Image.quantize(palette=...) to map any frame onto
    this fixed palette (see docs/CIN2_FORMAT.md on why the palette must
    be fixed for the whole movie rather than per-frame)."""
    if not frames:
        raise ValueError("no frames to sample")

    step = max(1, len(frames) // max(1, sample_count))
    sampled = frames[::step][:sample_count]

    tile_w, tile_h = fmt.WIDTH, fmt.HEIGHT
    cols = min(len(sampled), 8)
    rows = (len(sampled) + cols - 1) // cols
    sheet = Image.new("RGB", (tile_w * cols, tile_h * rows))
    for i, img in enumerate(sampled):
        x = (i % cols) * tile_w
        y = (i // cols) * tile_h
        sheet.paste(img.convert("RGB"), (x, y))

    return sheet.quantize(colors=fmt.PALETTE_ENTRIES, method=getattr(Image, "Quantize", Image).MEDIANCUT)


def palette_image_to_rgb565(palette_image: Image.Image) -> list[int]:
    raw = palette_image.getpalette()
    if raw is None:
        raise ValueError("palette_image has no palette")

    entries = []
    for i in range(fmt.PALETTE_ENTRIES):
        offset = i * 3
        if offset + 2 < len(raw):
            r, g, b = raw[offset], raw[offset + 1], raw[offset + 2]
        else:
            r = g = b = 0
        entries.append(rgb888_to_rgb565(r, g, b))
    return entries


def quantize_frame(image: Image.Image, palette_image: Image.Image) -> list[int]:
    """Maps an RGB frame onto the fixed global palette, returning
    WIDTH*HEIGHT indices (0..15), row-major top-to-bottom."""
    rgb = image.convert("RGB")
    if rgb.size != (fmt.WIDTH, fmt.HEIGHT):
        rgb = rgb.resize((fmt.WIDTH, fmt.HEIGHT), getattr(Image, "Resampling", Image).LANCZOS)
    quantized = rgb.quantize(palette=palette_image, dither=getattr(Image, "Dither", Image).FLOYDSTEINBERG)
    indices = list(quantized.tobytes())
    # Floyd-Steinberg dithering with a quantize()-supplied palette can
    # occasionally emit an index past the palette's actual (possibly
    # <16, if the source sheet had fewer than 16 distinct colors) entry
    # count; clamp defensively since src/decode.c interprets any 4-bit
    # value 0..15 as a valid palette slot with no bounds check of its
    # own (the display hardware just shows whatever color is there).
    return [min(i, fmt.PALETTE_ENTRIES - 1) for i in indices]


def encode(video_path: Path, output_path: Path, fps_num: int, fps_den: int,
           palette_samples: int, start: Optional[float], duration: Optional[float]) -> None:
    with tempfile.TemporaryDirectory(prefix="cin2_frames_") as tmp:
        tmp_dir = Path(tmp)
        frame_paths = extract_frames(video_path, tmp_dir, fps_num, fps_den, start, duration)

        sample_step = max(1, len(frame_paths) // max(1, palette_samples))
        sample_images = [Image.open(p).convert("RGB")
                          for p in frame_paths[::sample_step][:palette_samples]]
        palette_image = build_global_palette(sample_images, palette_samples)
        palette = palette_image_to_rgb565(palette_image)

        header = fmt.Cin2Header(
            width=fmt.WIDTH, height=fmt.HEIGHT, fps_num=fps_num, fps_den=fps_den,
            frame_count=len(frame_paths), palette=palette,
        )

        with open(output_path, "wb") as out:
            out.write(fmt.build_header(header))
            for path in frame_paths:
                with Image.open(path) as img:
                    indices = quantize_frame(img, palette_image)
                out.write(fmt.pack_frame(indices))

    total_bytes = fmt.HEADER_BYTES + len(frame_paths) * fmt.PACKED_BYTES
    duration_s = len(frame_paths) * fps_den / fps_num
    throughput_kib_s = (fmt.FRAME_SECTORS * fmt.SECTOR_BYTES * fps_num / fps_den) / 1024
    print(f"wrote {output_path}: {len(frame_paths)} frames, "
          f"{duration_s:.1f}s @ {fps_num}/{fps_den} fps, {total_bytes} bytes "
          f"({total_bytes / 1024:.1f} KiB), ~{throughput_kib_s:.1f} KiB/s required "
          "(CE Toolchain docs report ~262-273 KiB/s for tested USB drives)")


def parse_fps(value: str) -> Tuple[int, int]:
    if "/" in value:
        num_str, den_str = value.split("/", 1)
        return int(num_str), int(den_str)
    return int(value), 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("video", type=Path, help="input video file (anything ffmpeg can decode)")
    parser.add_argument("output", type=Path, help="output .bin file to raw-copy to a USB drive")
    parser.add_argument("--fps", type=parse_fps, default=(24, 1),
                         help="target frame rate as N or N/D, e.g. 24 or 24000/1001 (default: 24)")
    parser.add_argument("--palette-samples", type=int, default=32,
                         help="number of frames sampled to build the global "
                              "16-color palette (default: 32)")
    parser.add_argument("--start", type=float, default=None, help="start offset in seconds")
    parser.add_argument("--duration", type=float, default=None, help="duration in seconds")
    args = parser.parse_args(argv)

    fps_num, fps_den = args.fps
    if fps_num <= 0 or fps_den <= 0:
        parser.error("--fps must be positive")

    encode(args.video, args.output, fps_num, fps_den, args.palette_samples,
           args.start, args.duration)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
