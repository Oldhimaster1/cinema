#!/usr/bin/env python3
"""Encodes a video into the Cinema v2 (CIN2) format for the TI-84 Plus CE.

Single-file, no sibling modules required (just this script + ffmpeg on
PATH + `pip install pillow`). See docs/CIN2_FORMAT.md in the Cinema repo
for the on-disk format this produces: a 512-byte header (magic,
resolution, frame rate, frame count, one shared 16-color palette)
followed by 160x96 packed-4-bit frames, 15 sectors each.

Usage:

    python3 encode_cin2.py input.mp4 output.bin \\
        [--fps 24] [--palette-samples 32] [--start 0] [--duration 60] \\
        [--jobs N]

Then write output.bin to a USB drive starting at LBA 0/byte 0, e.g.:

    sudo dd if=output.bin of=/dev/sdX bs=1M conv=fsync

(Linux/macOS) or with HDD Raw Copy Tool on Windows -- as a *raw image*,
not a normal file copy; the calculator never sees a filesystem or a
filename, so the extension you give `output.bin` doesn't matter.

--- Performance notes ---

Two things made earlier versions of this script slow on full-length
video, both fixed here:

1. Frames used to be extracted to individual PNG files on disk (one
   ffmpeg PNG-encode + one Pillow PNG-decode per frame) and read back.
   PNG's DEFLATE compression is the most expensive step in the whole
   pipeline, paid twice, for every single frame. This version pipes raw
   rgb24 frames from ffmpeg directly into Python over a single stdout
   pipe -- no compression, no per-frame files, one ffmpeg process for
   the whole movie.

2. Per-frame palette quantization + dithering (Pillow, CPU-bound) used
   to run single-threaded, one frame at a time. It's now spread across
   a multiprocessing pool (--jobs, default: all CPU cores), since each
   frame quantizes independently once the movie's palette is fixed.

Building the global palette still needs a representative sample spread
across the whole movie, and that can't be known until the movie's been
fully read once -- so there are two ffmpeg passes: a cheap, low-fps
sampling pass (bounded to --palette-samples frames) to build the
palette, then one full-rate pass to encode every frame. Neither pass
touches disk for frame data; only the final packed output is written.
"""
from __future__ import annotations

import argparse
import io
import multiprocessing
import os
import shutil
import struct
import subprocess
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import IO, Iterator, List, Optional, Sequence, Tuple

from PIL import Image

# --- CIN2 format constants (mirrors docs/CIN2_FORMAT.md / tools/cin2_format.py
# in the Cinema repo -- duplicated here, not imported, so this script has
# no sibling-file dependency). If you change the format, update both. ---

MAGIC = b"CIN2"
VERSION = 2
HEADER_BYTES = 512
CRC_BYTES = 22
PALETTE_ENTRIES = 16

WIDTH = 160
HEIGHT = 96
PACKED_BYTES = (WIDTH * HEIGHT) // 2
FRAME_SECTORS = 15
SECTOR_BYTES = 512
assert FRAME_SECTORS * SECTOR_BYTES == PACKED_BYTES

RAW_FRAME_BYTES = WIDTH * HEIGHT * 3  # rgb24 from ffmpeg


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


@dataclass
class Cin2Header:
    width: int
    height: int
    fps_num: int
    fps_den: int
    frame_count: int
    palette: Sequence[int] = field(default_factory=lambda: [0] * PALETTE_ENTRIES)


def build_header(header: Cin2Header) -> bytes:
    if len(header.palette) != PALETTE_ENTRIES:
        raise ValueError(f"palette must have exactly {PALETTE_ENTRIES} entries")

    buf = bytearray(HEADER_BYTES)
    buf[0:4] = MAGIC
    buf[4] = VERSION
    buf[5] = 0  # flags, reserved
    struct.pack_into(
        "<HHLLL", buf, 6,
        header.width, header.height, header.fps_num, header.fps_den,
        header.frame_count,
    )
    struct.pack_into("<L", buf, 22, crc32(bytes(buf[:CRC_BYTES])))
    for i, entry in enumerate(header.palette):
        struct.pack_into("<H", buf, 26 + i * 2, entry)
    return bytes(buf)


def pack_frame(indices: Sequence[int]) -> bytes:
    """indices: WIDTH*HEIGHT palette indices (0..15), row-major. Returns
    PACKED_BYTES bytes: {left<<4 | right} per horizontally adjacent pair."""
    if len(indices) != WIDTH * HEIGHT:
        raise ValueError(f"expected {WIDTH * HEIGHT} indices, got {len(indices)}")

    out = bytearray(PACKED_BYTES)
    for i in range(0, len(indices), 2):
        left = indices[i]
        right = indices[i + 1]
        out[i // 2] = ((left & 0x0F) << 4) | (right & 0x0F)
    return bytes(out)


def rgb888_to_rgb1555(r: int, g: int, b: int) -> int:
    """Packs 8-bit RGB into the 1555 layout gfx_SetPalette actually
    expects (5 bits each of R/G/B, top bit unused) -- the same bit
    layout as the real graphx.h's gfx_RGBTo1555 macro. Not RGB565: CE's
    GraphX has no 565 palette format at all, only 1555 (confirmed by
    real-hardware testing showing wrong/inverted-looking colors when
    this was originally packed as 565)."""
    return ((r & 0xF8) << 7) | ((g & 0xF8) << 2) | (b >> 3)


# --- ffmpeg/ffprobe plumbing -------------------------------------------

def _require_ffmpeg() -> None:
    if shutil.which("ffmpeg") is None:
        raise RuntimeError("ffmpeg not found on PATH -- required to decode the input video")


def probe_duration_seconds(video_path: Path) -> Optional[float]:
    """Best-effort: returns None (never raises) if ffprobe is missing or
    the duration can't be determined -- callers fall back to a sane
    default. This is only used to pick a sampling rate for the palette
    pass, so an approximate/missing duration doesn't affect correctness,
    only how evenly the palette samples are spread across the movie."""
    if shutil.which("ffprobe") is None:
        return None
    try:
        result = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "default=noprint_wrappers=1:nokey=1", str(video_path)],
            capture_output=True, text=True, timeout=30,
        )
        if result.returncode != 0:
            return None
        return float(result.stdout.strip())
    except (subprocess.SubprocessError, ValueError, OSError):
        return None


def _read_exact(stream: IO[bytes], n: int) -> Optional[bytes]:
    """Reads exactly n bytes from a pipe (a single .read(n) call on a
    pipe can return short even mid-stream). Returns None on a clean EOF
    between frames, raises on a truncated final frame."""
    chunks = []
    remaining = n
    while remaining > 0:
        chunk = stream.read(remaining)
        if not chunk:
            if remaining == n:
                return None
            raise IOError(f"ffmpeg output ended mid-frame ({n - remaining}/{n} bytes read)")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


#: Upper bound, in seconds, on how much of the source the palette-sampling
#: pass will decode. This is what keeps pass 1 cheap for a long movie: the
#: alternative of spreading samples across the *entire* runtime still
#: requires decoding the entire runtime (the fps filter only decides which
#: already-decoded frames to keep), so pass 1's cost would scale with movie
#: length exactly like pass 2's does, defeating the point of a "cheap"
#: sampling pass. A tried-and-reverted alternative -- seeking to `count`
#: separate timestamps spread across the whole runtime with one small
#: ffmpeg process per sample -- was *slower* than this in measurement: with
#: samples spaced closer than the source's keyframe interval (common for
#: typical long-GOP encodes), most seeks land mid-GOP and still decode
#: forward from the nearest keyframe, paying that cost once per sample
#: instead of once total, on top of per-process startup overhead. Capping
#: the decoded *range* instead of changing *how* it's sampled is what
#: actually bounds the cost, at the tradeoff of the palette reflecting
#: only the first PALETTE_PROBE_SECONDS_CAP of the movie rather than the
#: whole thing -- a quality tradeoff, not a correctness one, and one any
#: single fixed-size palette for a whole movie already makes to some degree.
PALETTE_PROBE_SECONDS_CAP = 60.0


def stream_raw_frames(video_path: Path, fps_num: int, fps_den: int,
                       start: Optional[float], duration: Optional[float],
                       max_frames: Optional[int] = None) -> Iterator[Image.Image]:
    """Runs ffmpeg once, scaled+resampled to WIDTHxHEIGHT @ fps_num/fps_den,
    and yields decoded frames as they arrive over a single stdout pipe --
    no intermediate files, no PNG. Raises subprocess.CalledProcessError if
    ffmpeg exits nonzero (e.g. an unreadable/non-video input)."""
    _require_ffmpeg()

    cmd = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error"]
    if start is not None:
        cmd += ["-ss", str(start)]
    cmd += ["-i", str(video_path)]
    if duration is not None:
        cmd += ["-t", str(duration)]
    cmd += ["-vf", f"fps={fps_num}/{fps_den},scale={WIDTH}:{HEIGHT}:flags=lanczos"]
    if max_frames is not None:
        cmd += ["-frames:v", str(max_frames)]
    cmd += ["-pix_fmt", "rgb24", "-f", "rawvideo", "-"]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert proc.stdout is not None and proc.stderr is not None
    try:
        while True:
            raw = _read_exact(proc.stdout, RAW_FRAME_BYTES)
            if raw is None:
                break
            yield Image.frombytes("RGB", (WIDTH, HEIGHT), raw)
    finally:
        proc.stdout.close()
        stderr = proc.stderr.read()
        proc.stderr.close()
        returncode = proc.wait()
        if returncode != 0:
            raise subprocess.CalledProcessError(returncode, cmd, stderr=stderr)


# --- palette + quantization (unchanged interfaces -- unit-tested directly
# with in-memory PIL Images, independent of how frames are sourced) -----

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

    tile_w, tile_h = WIDTH, HEIGHT
    cols = min(len(sampled), 8)
    rows = (len(sampled) + cols - 1) // cols
    sheet = Image.new("RGB", (tile_w * cols, tile_h * rows))
    for i, img in enumerate(sampled):
        x = (i % cols) * tile_w
        y = (i // cols) * tile_h
        sheet.paste(img.convert("RGB"), (x, y))

    return sheet.quantize(colors=PALETTE_ENTRIES, method=Image.Quantize.MEDIANCUT)


def palette_image_to_rgb1555(palette_image: Image.Image) -> List[int]:
    raw = palette_image.getpalette()
    if raw is None:
        raise ValueError("palette_image has no palette")

    entries = []
    for i in range(PALETTE_ENTRIES):
        offset = i * 3
        if offset + 2 < len(raw):
            r, g, b = raw[offset], raw[offset + 1], raw[offset + 2]
        else:
            r = g = b = 0
        entries.append(rgb888_to_rgb1555(r, g, b))
    return entries


def quantize_frame(image: Image.Image, palette_image: Image.Image) -> List[int]:
    """Maps an RGB frame onto the fixed global palette, returning
    WIDTH*HEIGHT indices (0..15), row-major top-to-bottom."""
    rgb = image.convert("RGB")
    if rgb.size != (WIDTH, HEIGHT):
        rgb = rgb.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    quantized = rgb.quantize(palette=palette_image, dither=Image.Dither.FLOYDSTEINBERG)
    indices = list(quantized.tobytes())
    # Floyd-Steinberg dithering with a quantize()-supplied palette can
    # occasionally emit an index past the palette's actual (possibly
    # <16, if the source sheet had fewer than 16 distinct colors) entry
    # count; clamp defensively since the calculator's decoder interprets
    # any 4-bit value 0..15 as a valid palette slot with no bounds check
    # of its own (the display hardware just shows whatever color is there).
    return [min(i, PALETTE_ENTRIES - 1) for i in indices]


# --- parallel per-frame quantize+pack -----------------------------------
# multiprocessing worker: each pool process reconstructs the (small,
# fixed) palette image once via _pool_init, then quantizes+packs whatever
# raw frame bytes it's handed. Frames cross the process boundary as raw
# bytes (not PIL Image objects) to sidestep any Pillow-pickling ambiguity.

_worker_palette_image: Optional[Image.Image] = None


def _pool_init(palette_png_bytes: bytes) -> None:
    global _worker_palette_image
    _worker_palette_image = Image.open(io.BytesIO(palette_png_bytes))
    _worker_palette_image.load()


def _pool_quantize_and_pack(raw_rgb: bytes) -> bytes:
    assert _worker_palette_image is not None
    image = Image.frombytes("RGB", (WIDTH, HEIGHT), raw_rgb)
    return pack_frame(quantize_frame(image, _worker_palette_image))


def _palette_image_to_png_bytes(palette_image: Image.Image) -> bytes:
    buf = io.BytesIO()
    palette_image.save(buf, format="PNG")
    return buf.getvalue()


# --- encode --------------------------------------------------------------

def encode(video_path: Path, output_path: Path, fps_num: int, fps_den: int,
           palette_samples: int, start: Optional[float], duration: Optional[float],
           jobs: Optional[int] = None) -> int:
    """Returns the number of frames written. Writes to
    output_path.with_name(output_path.name + ".partial") first and only
    renames it to output_path after a self-check confirms the header and
    file size are consistent -- a filename ending in the real output name
    should therefore never exist half-written. On any failure (ffmpeg
    error, disk full, permission error, verification failure), the
    partial file is removed and output_path is left untouched (not
    created, and not overwritten if it already existed)."""
    partial_path = output_path.with_name(output_path.name + ".partial")
    jobs = jobs if jobs and jobs > 0 else (os.cpu_count() or 1)

    try:
        # --- pass 1: cheap, bounded sample for the global palette ---
        # Bound how much of the source this pass decodes (see
        # PALETTE_PROBE_SECONDS_CAP) rather than trying to spread samples
        # across the whole runtime -- that's what actually keeps this
        # pass's cost independent of movie length.
        probed = probe_duration_seconds(video_path)
        requested_span = duration if duration is not None else probed
        probe_span = (min(requested_span, PALETTE_PROBE_SECONDS_CAP)
                      if requested_span and requested_span > 0
                      else PALETTE_PROBE_SECONDS_CAP)

        # fps = palette_samples / probe_span, as an exact fraction scaled
        # by 1000x for sub-second precision -- e.g. probe_span=0.05s, 2
        # samples must come out to 40fps (one frame every 0.025s) to fit
        # both samples in the probed span at all; rounding probe_span to
        # whole seconds first (an earlier version of this) collapses any
        # span under ~1s to 0 and produces no sample frames whatsoever.
        sample_fps_num = max(1, round(palette_samples * 1000))
        sample_fps_den = max(1, round(probe_span * 1000))

        sample_frames = list(stream_raw_frames(
            video_path, sample_fps_num, sample_fps_den, start, probe_span,
            max_frames=palette_samples,
        ))
        palette_image = build_global_palette(sample_frames, palette_samples)
        palette = palette_image_to_rgb1555(palette_image)
        del sample_frames  # bounded (<= palette_samples), but no reason to hold it longer

        # --- pass 2: full-rate stream, quantized+packed in parallel ---
        placeholder_header = build_header(Cin2Header(
            width=WIDTH, height=HEIGHT, fps_num=fps_num, fps_den=fps_den,
            frame_count=0, palette=palette,
        ))
        palette_png_bytes = _palette_image_to_png_bytes(palette_image)

        frame_count = 0
        with open(partial_path, "wb") as out:
            out.write(placeholder_header)  # patched with the real frame_count below

            raw_frames = (img.tobytes() for img in
                          stream_raw_frames(video_path, fps_num, fps_den, start, duration))

            if jobs == 1:
                for raw in raw_frames:
                    out.write(_pool_quantize_and_pack_single(raw, palette_image))
                    frame_count += 1
            else:
                with multiprocessing.Pool(
                    processes=jobs, initializer=_pool_init, initargs=(palette_png_bytes,)
                ) as pool:
                    for packed in pool.imap(_pool_quantize_and_pack, raw_frames, chunksize=8):
                        out.write(packed)
                        frame_count += 1

            if frame_count == 0:
                raise RuntimeError("no frames were extracted -- check the input video/time range")

            out.seek(0)
            out.write(build_header(Cin2Header(
                width=WIDTH, height=HEIGHT, fps_num=fps_num, fps_den=fps_den,
                frame_count=frame_count, palette=palette,
            )))

        _self_check(partial_path, frame_count)
        partial_path.replace(output_path)
    except BaseException:
        partial_path.unlink(missing_ok=True)
        raise

    total_bytes = HEADER_BYTES + frame_count * PACKED_BYTES
    duration_s = frame_count * fps_den / fps_num
    throughput_kib_s = (FRAME_SECTORS * SECTOR_BYTES * fps_num / fps_den) / 1024
    print(f"wrote {output_path}: {frame_count} frames, "
          f"{duration_s:.1f}s @ {fps_num}/{fps_den} fps, {total_bytes} bytes "
          f"({total_bytes / 1024:.1f} KiB), ~{throughput_kib_s:.1f} KiB/s required "
          "(CE Toolchain docs report ~262-273 KiB/s for tested USB drives)")
    return frame_count


def _pool_quantize_and_pack_single(raw_rgb: bytes, palette_image: Image.Image) -> bytes:
    """--jobs 1 path: same work as _pool_quantize_and_pack, no pool."""
    image = Image.frombytes("RGB", (WIDTH, HEIGHT), raw_rgb)
    return pack_frame(quantize_frame(image, palette_image))


def _self_check(path: Path, expected_frame_count: int) -> None:
    """Lightweight, self-contained sanity check -- re-reads what was just
    written and confirms the header parses, the CRC matches, and the
    file size matches the formula for expected_frame_count exactly. Not
    a full validator (see tools/verify_cin2.py in the Cinema repo for
    that); this only needs to catch "the file we just wrote is not the
    file we meant to write" before it gets renamed into place."""
    raw = path.read_bytes()
    if len(raw) < HEADER_BYTES:
        raise RuntimeError("self-check failed: output shorter than the header")

    header = raw[:HEADER_BYTES]
    if header[0:4] != MAGIC:
        raise RuntimeError("self-check failed: bad magic in written header")
    stored_crc = struct.unpack_from("<L", header, 22)[0]
    if stored_crc != crc32(header[:CRC_BYTES]):
        raise RuntimeError("self-check failed: header CRC mismatch")
    frame_count = struct.unpack_from("<L", header, 18)[0]
    if frame_count != expected_frame_count:
        raise RuntimeError(
            f"self-check failed: header frame_count {frame_count} != {expected_frame_count}")

    expected_size = HEADER_BYTES + frame_count * PACKED_BYTES
    if len(raw) != expected_size:
        raise RuntimeError(
            f"self-check failed: file size {len(raw)} != expected {expected_size}")


# --- CLI -------------------------------------------------------------

def parse_fps(value: str) -> Tuple[int, int]:
    if "/" in value:
        num_str, den_str = value.split("/", 1)
        return int(num_str), int(den_str)
    return int(value), 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("video", type=Path, help="input video file (anything ffmpeg can decode)")
    parser.add_argument("output", type=Path, help="output file to raw-copy to a USB drive")
    parser.add_argument("--fps", type=parse_fps, default=(24, 1),
                         help="target frame rate as N or N/D, e.g. 24 or 24000/1001 (default: 24)")
    parser.add_argument("--palette-samples", type=int, default=32,
                         help="number of frames sampled to build the global "
                              "16-color palette (default: 32)")
    parser.add_argument("--start", type=float, default=None, help="start offset in seconds")
    parser.add_argument("--duration", type=float, default=None, help="duration in seconds")
    parser.add_argument("--jobs", type=int, default=None,
                         help="parallel worker processes for quantization "
                              "(default: all CPU cores; 1 disables the pool)")
    args = parser.parse_args(argv)

    fps_num, fps_den = args.fps
    if fps_num <= 0 or fps_den <= 0:
        parser.error("--fps must be positive")

    encode(args.video, args.output, fps_num, fps_den, args.palette_samples,
           args.start, args.duration, args.jobs)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
