#!/usr/bin/env python3
"""Encodes a video into the Cinema v2 (CIN2) format for the TI-84 Plus CE.

Single-file, no sibling modules required (just this script + ffmpeg on
PATH + `pip install pillow`). See docs/CIN2_FORMAT.md in the Cinema repo
for the on-disk format this produces: a 512-byte header (magic,
resolution, frame rate, frame count, one shared 16-color palette)
followed by 160x96 frames of one byte per pixel (0..15, unpacked), 30
sectors each.

Usage:

    python3 encode_cin2.py input.mp4 output.bin \\
        [--fps 15] [--palette-samples 32] [--start 0] [--duration 60] \\
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
# One byte per pixel (0..15), not bit-packed -- see docs/CIN2_FORMAT.md's
# "Why a new format" section for why v2 dropped 4-bit packing after
# real-hardware testing showed the CPU cost of unpacking it back out on
# the ez80 core cost more than the packing saved.
FRAME_BYTES = WIDTH * HEIGHT
FRAME_SECTORS = 30
SECTOR_BYTES = 512
assert FRAME_SECTORS * SECTOR_BYTES == FRAME_BYTES

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


def encode_frame(indices: Sequence[int]) -> bytes:
    """indices: WIDTH*HEIGHT palette indices (0..15), row-major. Returns
    FRAME_BYTES bytes, one per pixel, unpacked -- this is exactly what
    lands in the calculator's sprite buffer straight off the USB read."""
    if len(indices) != WIDTH * HEIGHT:
        raise ValueError(f"expected {WIDTH * HEIGHT} indices, got {len(indices)}")
    return bytes(min(max(i, 0), 15) for i in indices)


def rgb888_to_rgb1555(r: int, g: int, b: int) -> int:
    """Packs 8-bit RGB into the exact RGB1555 layout used by GraphX."""
    return ((r & 0xF8) << 7) | ((g & 0xF8) << 2) | (b >> 3)


def rgb1555_to_rgb888(value: int) -> Tuple[int, int, int]:
    """Returns the exact 8-bit RGB color displayed for an RGB1555 entry.

    Five-bit channels are expanded by bit replication, matching the usual
    0..31 to 0..255 mapping while preserving both endpoints exactly.
    """
    r5 = (value >> 10) & 31
    g5 = (value >> 5) & 31
    b5 = value & 31
    return ((r5 << 3) | (r5 >> 2),
            (g5 << 3) | (g5 >> 2),
            (b5 << 3) | (b5 >> 2))


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


#: The global 16-color palette must represent the complete selected clip.
#: Earlier code capped palette analysis to the first 60 seconds, so colors
#: introduced later in a movie could collapse toward neutral entries and look
#: like a gradual fade to grayscale. We now sample uniformly across the full
#: selected duration. This requires a full low-output-rate decode pass, but it
#: keeps chroma representative from beginning to end.
PALETTE_PROBE_SECONDS_CAP = None

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
    cmd += ["-map", "0:v:0", "-an", "-sn", "-dn"]
    cmd += [
        "-vf",
        f"fps={fps_num}/{fps_den}:round=near:eof_action=pass,"
        f"scale={WIDTH}:{HEIGHT}:flags=lanczos",
    ]
    if max_frames is not None:
        cmd += ["-frames:v", str(max_frames)]
    # The fps filter alone owns frame selection. Passthrough prevents the
    # output stage from duplicating/dropping a second time or relabeling timing.
    cmd += ["-fps_mode", "passthrough", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]

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

def _palette_image_from_rgb1555(entries: Sequence[int]) -> Image.Image:
    """Builds a Pillow palette made from exact calculator-display colors."""
    if len(entries) != PALETTE_ENTRIES:
        raise ValueError("palette must have exactly 16 entries")
    raw: List[int] = []
    for entry in entries:
        raw.extend(rgb1555_to_rgb888(entry))
    raw.extend([0] * (256 * 3 - len(raw)))
    image = Image.new("P", (1, 1))
    image.putpalette(raw)
    return image


def _candidate_rgb1555_colors(sheet: Image.Image) -> List[int]:
    """Returns deterministic, frequency-ranked source colors at LCD precision."""
    reduced = sheet.quantize(colors=256, method=Image.Quantize.MEDIANCUT,
                             dither=Image.Dither.NONE)
    counts = reduced.getcolors(maxcolors=256) or []
    palette = reduced.getpalette() or []
    ranked = sorted(counts, key=lambda item: (-item[0], item[1]))
    result: List[int] = []
    seen = set()
    for _count, index in ranked:
        offset = index * 3
        if offset + 2 >= len(palette):
            continue
        value = rgb888_to_rgb1555(palette[offset], palette[offset + 1], palette[offset + 2])
        if value not in seen:
            seen.add(value)
            result.append(value)
    return result


def build_global_palette(frames: Sequence[Image.Image], sample_count: int) -> Image.Image:
    """Builds a deterministic RGB1555-aware global palette.

    The old encoder chose indices against 24-bit colors, then rounded the
    stored palette to RGB1555 afterward. This version first rounds candidate
    colors to RGB1555, reconstructs the exact display RGB values, and makes
    Pillow quantize against those exact values. Duplicate RGB1555 entries are
    replaced with frequency-ranked source candidates whenever the sampled
    source contains enough distinct colors.
    """
    if not frames:
        raise ValueError("no frames to sample")
    step = max(1, len(frames) // max(1, sample_count))
    sampled = frames[::step][:sample_count]
    cols = min(len(sampled), 8)
    rows = (len(sampled) + cols - 1) // cols
    sheet = Image.new("RGB", (WIDTH * cols, HEIGHT * rows))
    for i, img in enumerate(sampled):
        sheet.paste(img.convert("RGB"), ((i % cols) * WIDTH, (i // cols) * HEIGHT))

    base = sheet.quantize(colors=PALETTE_ENTRIES, method=Image.Quantize.MEDIANCUT,
                          dither=Image.Dither.NONE)
    base_raw = base.getpalette() or []
    entries: List[int] = []
    seen = set()
    base_entry_count = min(PALETTE_ENTRIES, len(base_raw) // 3)
    for i in range(base_entry_count):
        offset = i * 3
        value = rgb888_to_rgb1555(base_raw[offset], base_raw[offset + 1], base_raw[offset + 2])
        if value not in seen:
            entries.append(value)
            seen.add(value)
    for value in _candidate_rgb1555_colors(sheet):
        if len(entries) >= PALETTE_ENTRIES:
            break
        if value not in seen:
            entries.append(value)
            seen.add(value)
    while len(entries) < PALETTE_ENTRIES:
        entries.append(entries[-1] if entries else 0)
    return _palette_image_from_rgb1555(entries)


def palette_image_to_rgb1555(palette_image: Image.Image) -> List[int]:
    raw = palette_image.getpalette()
    if raw is None:
        raise ValueError("palette_image has no palette")
    return [rgb888_to_rgb1555(raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2])
            for i in range(PALETTE_ENTRIES)]


def quantize_frame(image: Image.Image, palette_image: Image.Image,
                   dither_mode: str = "clean") -> List[int]:
    """Maps a frame to the fixed palette.

    legacy preserves full Floyd-Steinberg diffusion. clean disables error
    diffusion, removing the isolated-pixel noise and temporal crawling it can
    create on flat surfaces. Clean is the default for both the CLI and direct API calls;
    legacy Floyd-Steinberg output remains available explicitly.
    """
    rgb = image.convert("RGB")
    if rgb.size != (WIDTH, HEIGHT):
        rgb = rgb.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    if dither_mode == "legacy":
        dither = Image.Dither.FLOYDSTEINBERG
    elif dither_mode == "clean":
        dither = Image.Dither.NONE
    else:
        raise ValueError(f"unknown dither mode: {dither_mode}")
    quantized = rgb.quantize(palette=palette_image, dither=dither)
    return [min(i, PALETTE_ENTRIES - 1) for i in quantized.tobytes()]


def automatic_palette_samples(duration_seconds: Optional[float]) -> int:
    """Chooses a bounded sample count appropriate for the selected span."""
    if duration_seconds is None or duration_seconds <= 0:
        return 64
    if duration_seconds <= 5 * 60:
        return 32
    if duration_seconds <= 30 * 60:
        return 64
    if duration_seconds <= 90 * 60:
        return 128
    return 256

# --- parallel per-frame quantize+pack -----------------------------------
# multiprocessing worker: each pool process reconstructs the (small,
# fixed) palette image once via _pool_init, then quantizes+packs whatever
# raw frame bytes it's handed. Frames cross the process boundary as raw
# bytes (not PIL Image objects) to sidestep any Pillow-pickling ambiguity.

_worker_palette_image: Optional[Image.Image] = None
_worker_dither_mode = "clean"


def _pool_init(palette_png_bytes: bytes, dither_mode: str) -> None:
    global _worker_palette_image, _worker_dither_mode
    _worker_palette_image = Image.open(io.BytesIO(palette_png_bytes))
    _worker_palette_image.load()
    _worker_dither_mode = dither_mode


def _pool_quantize_and_pack(raw_rgb: bytes) -> bytes:
    assert _worker_palette_image is not None
    image = Image.frombytes("RGB", (WIDTH, HEIGHT), raw_rgb)
    return encode_frame(quantize_frame(image, _worker_palette_image, _worker_dither_mode))


def _palette_image_to_png_bytes(palette_image: Image.Image) -> bytes:
    buf = io.BytesIO()
    palette_image.save(buf, format="PNG")
    return buf.getvalue()


# --- encode --------------------------------------------------------------

def encode(video_path: Path, output_path: Path, fps_num: int, fps_den: int,
           palette_samples: int, start: Optional[float], duration: Optional[float],
           jobs: Optional[int] = None, dither_mode: str = "clean") -> int:
    """Returns the number of frames written. Writes to
    output_path.with_name(output_path.name + ".partial") first and only
    renames it to output_path after a self-check confirms the header and
    file size are consistent -- a filename ending in the real output name
    should therefore never exist half-written. On any failure (ffmpeg
    error, disk full, permission error, verification failure), the
    partial file is removed and output_path is left untouched (not
    created, and not overwritten if it already existed)."""
    partial_path = output_path.with_name(output_path.name + ".partial")
    jobs = jobs if jobs and jobs > 0 else min(4, os.cpu_count() or 1)  # measured exact-output winner on 4C/8T control machine

    try:
        # --- pass 1: cheap, bounded sample for the global palette ---
        # Bound how much of the source this pass decodes (see
        # PALETTE_PROBE_SECONDS_CAP) rather than trying to spread samples
        # across the whole runtime -- that's what actually keeps this
        # pass's cost independent of movie length.
        probed = probe_duration_seconds(video_path)
        requested_span = duration if duration is not None else probed
        if palette_samples <= 0:
            palette_samples = automatic_palette_samples(requested_span)
        # Valid media normally has a probeable duration. If ffprobe cannot
        # determine it, retain a bounded fallback so ffmpeg remains responsible
        # for reporting unreadable/corrupt input through its normal error path.
        probe_span = requested_span if requested_span and requested_span > 0 else 60.0

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
                    out.write(_pool_quantize_and_pack_single(raw, palette_image, dither_mode))
                    frame_count += 1
            else:
                with multiprocessing.Pool(
                    processes=jobs, initializer=_pool_init, initargs=(palette_png_bytes, dither_mode)
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
        encoded_duration = frame_count * fps_den / fps_num
        frame_period = fps_den / fps_num
        # Preserve real-time motion: lowering FPS must select fewer frames, not
        # retain source-frame count and play those frames at a lower header rate.
        # FFmpeg boundary rounding is allowed by at most two output frames.
        if (requested_span is not None and requested_span > 0
                and abs(encoded_duration - requested_span) > 2.0 * frame_period + 1e-6):
            raise RuntimeError(
                f"timing self-check failed: source span {requested_span:.6f}s, "
                f"encoded span {encoded_duration:.6f}s at {fps_num}/{fps_den} fps"
            )
        partial_path.replace(output_path)
    except BaseException:
        partial_path.unlink(missing_ok=True)
        raise

    total_bytes = HEADER_BYTES + frame_count * FRAME_BYTES
    duration_s = frame_count * fps_den / fps_num
    throughput_kib_s = (FRAME_SECTORS * SECTOR_BYTES * fps_num / fps_den) / 1024
    print(f"wrote {output_path}: {frame_count} frames, "
          f"{duration_s:.1f}s @ {fps_num}/{fps_den} fps, {total_bytes} bytes "
          f"({total_bytes / 1024:.1f} KiB), ~{throughput_kib_s:.1f} KiB/s required "
          "(CE Toolchain docs report ~262-273 KiB/s for tested USB drives)")
    return frame_count


def _pool_quantize_and_pack_single(raw_rgb: bytes, palette_image: Image.Image,
                                   dither_mode: str = "clean") -> bytes:
    """--jobs 1 path: same work as _pool_quantize_and_pack, no pool."""
    image = Image.frombytes("RGB", (WIDTH, HEIGHT), raw_rgb)
    return encode_frame(quantize_frame(image, palette_image, dither_mode))


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

    expected_size = HEADER_BYTES + frame_count * FRAME_BYTES
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
    parser.add_argument("video", type=Path,
                         help="input video file (anything ffmpeg can decode), or a "
                              "directory of them for batch mode (see --batch-ext)")
    parser.add_argument("output", type=Path,
                         help="output file to raw-copy to a USB drive, or an output "
                              "directory in batch mode (one .bin per input, same "
                              "basename) -- created if it doesn't exist")
    parser.add_argument("--batch-ext", default=".mp4,.mkv,.mov,.avi,.webm,.m4v",
                         help="comma-separated, case-insensitive extensions to pick up "
                              "when `video` is a directory (default: %(default)s)")
    parser.add_argument("--fps", type=parse_fps, default=(15, 1),
                         help="target frame rate as N or N/D, e.g. 24 or 24000/1001 "
                              "(default: 15 -- at 15,360 bytes/frame this fits the "
                              "CE Toolchain's documented ~262-273 KiB/s tested USB "
                              "throughput with headroom; 24fps needs ~360 KiB/s, "
                              "above that budget on a typical drive)")
    parser.add_argument("--palette-samples", type=int, default=0,
                         help="frames sampled for the global palette; 0 selects "
                              "32/64/128/256 automatically by duration (default: 0)")
    parser.add_argument("--dither", choices=("clean", "legacy"), default="clean",
                         help="clean removes error-diffusion speckles; legacy keeps "
                              "the previous Floyd-Steinberg output (default: clean)")
    parser.add_argument("--start", type=float, default=None, help="start offset in seconds")
    parser.add_argument("--duration", type=float, default=None, help="duration in seconds")
    parser.add_argument("--jobs", type=int, default=None,
                         help="parallel worker processes for quantization "
                              "(default: all CPU cores; 1 disables the pool)")
    parser.add_argument("--subtitles", type=Path, default=None,
                         help="optional SRT file; writes a matching .csu and JSON report")
    parser.add_argument("--subtitle-output", type=Path, default=None,
                         help="optional CSU output path (default: movie output with .csu suffix)")
    args = parser.parse_args(argv)

    fps_num, fps_den = args.fps
    if fps_num <= 0 or fps_den <= 0:
        parser.error("--fps must be positive")

    if args.video.is_dir():
        return batch_encode(args.video, args.output, args.batch_ext, fps_num, fps_den,
                             args.palette_samples, args.start, args.duration, args.jobs,
                             args.dither)

    frame_count = encode(args.video, args.output, fps_num, fps_den, args.palette_samples,
                         args.start, args.duration, args.jobs, args.dither)
    if args.subtitles is not None:
        from srt_to_csu import convert as convert_subtitles
        subtitle_output = args.subtitle_output or args.output.with_suffix(".csu")
        convert_subtitles(args.subtitles, subtitle_output, args.output.name,
                          frame_count, fps_num, fps_den, args.output.stat().st_size)
    return 0


def batch_encode(video_dir: Path, output_dir: Path, ext_list: str, fps_num: int, fps_den: int,
                  palette_samples: int, start: Optional[float], duration: Optional[float],
                  jobs: Optional[int], dither_mode: str = "clean") -> int:
    """Encodes every video file directly inside video_dir (not
    recursive) whose extension matches ext_list into output_dir, one
    .bin per input with the same basename. A single failing file (a
    corrupt video, an unreadable codec, ...) is reported and skipped
    rather than aborting the whole batch -- with more than a couple of
    videos to convert, that's the difference between "97 succeeded, 3
    need a look" and having to figure out which one file to remove
    before starting over. Returns 0 if at least one file succeeded and
    none failed, 1 if any failed (even if others succeeded), 2 if
    nothing matched at all."""
    extensions = {e.strip().lower() for e in ext_list.split(",") if e.strip()}
    videos = sorted(
        p for p in video_dir.iterdir()
        if p.is_file() and p.suffix.lower() in extensions
    )

    if not videos:
        print(f"no video files (extensions: {', '.join(sorted(extensions))}) found in {video_dir}",
              file=sys.stderr)
        return 2

    output_dir.mkdir(parents=True, exist_ok=True)

    succeeded = []
    failed = []
    for video_path in videos:
        out_path = output_dir / (video_path.stem + ".bin")
        print(f"--- {video_path.name} -> {out_path.name} ---")
        try:
            encode(video_path, out_path, fps_num, fps_den, palette_samples,
                   start, duration, jobs, dither_mode)
            succeeded.append(video_path.name)
        except Exception as exc:  # noqa: BLE001 -- one bad file must not sink the batch
            print(f"FAILED: {video_path.name}: {exc}", file=sys.stderr)
            failed.append(video_path.name)

    print(f"\nbatch complete: {len(succeeded)} succeeded, {len(failed)} failed"
          f" (of {len(videos)} found)")
    if failed:
        print("failed: " + ", ".join(failed), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
