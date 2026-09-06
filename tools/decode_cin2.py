#!/usr/bin/env python3
"""Decodes CIN2 images to PNG/preview so you can see exactly what the
calculator would display before writing anything to a USB drive.

Usage:
    python3 tools/decode_cin2.py movie.bin --frame 0 --out frame0.png
    python3 tools/decode_cin2.py movie.bin --frames 0:10 --out-dir frames/
    python3 tools/decode_cin2.py movie.bin --all --out-dir frames/
    python3 tools/decode_cin2.py movie.bin --preview out.mp4   # needs ffmpeg
    python3 tools/decode_cin2.py movie.bin --report

Each PNG is written at both native 160x96 ("*_native.png") and the
scaled 320x192 the calculator actually draws ("*_scaled.png", plain
nearest-neighbor 2x, matching src/player_v2.c's blit exactly).
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cin2_format as fmt  # noqa: E402


def read_movie(path: Path):
    raw = path.read_bytes()
    header = fmt.parse_header(raw[: fmt.HEADER_BYTES])
    return raw, header


def frame_indices(raw: bytes, header: fmt.Cin2Header, frame_number: int) -> list[int]:
    if frame_number >= header.frame_count:
        raise ValueError(f"frame {frame_number} >= frame_count {header.frame_count}")
    start = fmt.frame_lba(frame_number) * fmt.SECTOR_BYTES
    end = start + fmt.FRAME_BYTES
    if end > len(raw):
        raise ValueError(f"frame {frame_number} data truncated in file")
    return fmt.decode_frame(raw[start:end])


def indices_to_rgb_image(indices: list[int], header: fmt.Cin2Header) -> Image.Image:
    """Renders logical WIDTHxHEIGHT indices as an RGB image using the
    header's palette (RGB1555 -> RGB888, matching gfx_SetPalette's real
    entry format), i.e. the "native" 160x96 view."""
    rgb_palette = []
    for entry in header.palette:
        r = (entry >> 10) & 0x1F
        g = (entry >> 5) & 0x1F
        b = entry & 0x1F
        rgb_palette.append(((r * 255) // 31, (g * 255) // 31, (b * 255) // 31))

    img = Image.new("RGB", (fmt.WIDTH, fmt.HEIGHT))
    pixels = [rgb_palette[i] for i in indices]
    img.putdata(pixels)
    return img


def scale_2x_nearest(img: Image.Image) -> Image.Image:
    """Matches what the calculator actually displays: src/player_v2.c
    reads each frame's raw pixel bytes straight into a sprite (no decode
    step at all) and draws it with GraphX's gfx_ScaledSprite_NoClip(...,
    2, 2) -- plain nearest-neighbor 2x, no interpolation, same as this."""
    return img.resize((img.width * 2, img.height * 2), Image.Resampling.NEAREST)


def decode_frame_png(raw: bytes, header: fmt.Cin2Header, frame_number: int,
                      out_path: Path, scaled: bool) -> None:
    indices = frame_indices(raw, header, frame_number)
    img = indices_to_rgb_image(indices, header)
    if scaled:
        img = scale_2x_nearest(img)
    img.save(out_path)


def report(raw: bytes, header: fmt.Cin2Header, path: Path) -> str:
    duration = header.frame_count * header.fps_den / header.fps_num
    lines = [
        f"file: {path}",
        f"size: {len(raw)} bytes",
        f"resolution: {header.width}x{header.height} (native), "
        f"{header.width * 2}x{header.height * 2} (scaled on-screen)",
        f"fps: {header.fps_num}/{header.fps_den} "
        f"({header.fps_num / header.fps_den:.4f})",
        f"frame_count: {header.frame_count}",
        f"expected duration: {duration:.3f}s ({duration / 60:.2f} min)",
        f"palette: {[f'0x{p:04X}' for p in header.palette]}",
    ]
    return "\n".join(lines)


def build_preview_video(raw: bytes, header: fmt.Cin2Header, out_path: Path) -> None:
    if shutil.which("ffmpeg") is None:
        raise RuntimeError("ffmpeg not found on PATH -- required for --preview")

    with tempfile.TemporaryDirectory(prefix="cin2_preview_") as tmp:
        tmp_dir = Path(tmp)
        for f in range(header.frame_count):
            indices = frame_indices(raw, header, f)
            img = scale_2x_nearest(indices_to_rgb_image(indices, header))
            img.save(tmp_dir / f"frame_{f:08d}.png")

        subprocess.run(
            [
                "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
                "-framerate", f"{header.fps_num}/{header.fps_den}",
                "-i", str(tmp_dir / "frame_%08d.png"),
                "-pix_fmt", "yuv420p",
                str(out_path),
            ],
            check=True,
        )


def parse_frame_range(spec: str, frame_count: int) -> range:
    if ":" in spec:
        a, b = spec.split(":", 1)
        start = int(a) if a else 0
        stop = int(b) if b else frame_count
        return range(start, stop)
    n = int(spec)
    return range(n, n + 1)


def main(argv: Optional[list] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=Path)
    parser.add_argument("--frame", type=int, help="decode a single frame number")
    parser.add_argument("--frames", type=str, help="decode a range, e.g. 0:10")
    parser.add_argument("--all", action="store_true", help="decode every frame")
    parser.add_argument("--out", type=Path, help="output PNG path (single --frame only)")
    parser.add_argument("--out-dir", type=Path, help="output directory for multiple frames")
    parser.add_argument("--native-only", action="store_true",
                         help="skip writing the 2x-scaled PNG variant")
    parser.add_argument("--preview", type=Path, help="write a full-length preview video (needs ffmpeg)")
    parser.add_argument("--report", action="store_true", help="print a metadata report")
    args = parser.parse_args(argv)

    raw, header = read_movie(args.image)

    if args.report or not (args.frame is not None or args.frames or args.all or args.preview):
        print(report(raw, header, args.image))
        if not (args.frame is not None or args.frames or args.all or args.preview):
            return 0

    if args.preview:
        build_preview_video(raw, header, args.preview)
        print(f"wrote preview: {args.preview}")

    frames: list[int] = []
    if args.frame is not None:
        frames = [args.frame]
    elif args.frames:
        frames = list(parse_frame_range(args.frames, header.frame_count))
    elif args.all:
        frames = list(range(header.frame_count))

    if frames:
        if len(frames) == 1 and args.out:
            decode_frame_png(raw, header, frames[0], args.out, scaled=True)
            print(f"wrote {args.out}")
            if not args.native_only:
                native_out = args.out.with_name(args.out.stem + "_native" + args.out.suffix)
                decode_frame_png(raw, header, frames[0], native_out, scaled=False)
                print(f"wrote {native_out}")
        else:
            out_dir = args.out_dir or Path(".")
            out_dir.mkdir(parents=True, exist_ok=True)
            for f in frames:
                scaled_path = out_dir / f"frame_{f:06d}_scaled.png"
                decode_frame_png(raw, header, f, scaled_path, scaled=True)
                if not args.native_only:
                    native_path = out_dir / f"frame_{f:06d}_native.png"
                    decode_frame_png(raw, header, f, native_path, scaled=False)
            print(f"wrote {len(frames)} frame(s) to {out_dir}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
