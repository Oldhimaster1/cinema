# Cinema

USB Video Player for the TI-84 Plus CE  

![Demo 1](media/dreamworks.gif) ![Demo 2](media/test_drive.gif)

## Overview

Cinema is a video player application that allows you to watch videos on your TI-84 Plus CE calculator using a USB thumb drive.

Cinema now supports two on-disk formats, auto-detected from the drive:

- **v2 (CIN2, recommended):** packed 4-bit indexed color, one shared
  16-color palette, 24fps-capable (any rational frame rate, e.g. 24/1 or
  24000/1001). See [`docs/CIN2_FORMAT.md`](docs/CIN2_FORMAT.md) for why
  this exists and the exact on-disk layout.
- **v1 (legacy):** the original 256-color-per-frame format, still fully
  supported for existing drives.

## Installation Instructions (v2 / 24fps)

Cinema reads movie files directly off a normally-formatted FAT32 USB
drive, so **multiple movies can live on one drive** as separate files --
no need to dedicate a whole drive to a single video.

1. **Format your USB drive as FAT32** (a drive fresh out of the packaging,
   or reformatted in Windows/macOS/Linux as usual, works fine -- no
   special tooling needed).
2. **Encode your video(s):**
   ```
   pip install pillow
   python3 tools/encode_cin2.py input.mp4 output.bin --fps 24
   ```
   Requires `ffmpeg` on your PATH. `tools/encode_cin2.py` is a single
   self-contained script -- no sibling files needed, copy just that one
   file anywhere. It decodes the source once via a raw pipe (no PNG
   round-trip) and quantizes frames in parallel across all CPU cores by
   default (`--jobs N` to control that, `--jobs 1` for single-threaded).
   Use `--fps 24000/1001` for film-rate content, `--start`/`--duration`
   to trim, and `--palette-samples` to control how many frames are
   sampled when building the movie's global 16-color palette (see
   `--help`).
3. **Copy the output file(s) onto the drive**, in the root folder, with
   a `.bin` or `.cin` extension (either works -- the extension is only
   used to tell movie files apart from anything else on the drive, e.g.
   `movie1.bin`, `movie2.cin`). Copy as many as you like.
4. **Install on calculator** -- transfer `CINEMA.8xp` to your TI-84 Plus CE.

### Raw whole-device image (legacy / fallback)

If a drive has no FAT32 filesystem at all (e.g. wiped with `dd` and never
formatted), Cinema falls back to the original raw-image mode: write a
single `.bin` straight to the start of the device (`sudo dd if=output.bin
of=/dev/sdX bs=1M conv=fsync` on Linux/macOS, or [HDD Raw Copy
Tool](https://hddguru.com/software/HDD-Raw-Copy-Tool/) on Windows) and
Cinema plays that one movie directly. This only supports a single movie
per drive -- prefer the FAT32 workflow above for anything else.

### v1 (legacy) drives

Existing v1 drives produced by [FBin](https://github.com/will-dabeast09/fbin)
still work unmodified -- just write the binary as before. Cinema detects
the format automatically from the drive.

## Usage

1. Run CINEMA on your calculator and insert your prepared USB drive.
2. On a FAT32 drive, pick a movie from the on-screen list (Up/Down to
   move, Enter to select, Clear to exit) -- shown even if there's only
   one file, so you always see what's on the drive.
3. If you've watched this movie before, choose to resume or restart.
4. Playback begins automatically.

**File browser controls (FAT32 drives only):**

| Key           | Action                                    |
|---------------|--------------------------------------------|
| Up / Down     | Move the selection                         |
| Enter / 2nd   | Play the selected movie                    |
| Clear         | Exit without playing anything              |

**v2 controls:**

| Key           | Action                                    |
|---------------|--------------------------------------------|
| 2nd / Enter   | Pause / resume                             |
| Left / Right  | Seek 10s back / forward                    |
| Up / Down     | Seek 60s forward / back                    |
| 0             | Restart from the beginning                 |
| Mode          | Pin the on-screen overlay open/closed      |
| Clear         | Exit (saves resume state)                  |

A progress bar, elapsed/total time, live FPS, and per-frame decode cost
appear briefly on any keypress (in the black letterbox bar under the
video, so it never covers the picture), and stay up if pinned with
Mode. The decode-cost figure printed on exit and shown live in the
overlay is the number to watch when judging playback speed -- see
"Performance" below.

**v1 controls:** any key exits (no pause/seek -- the legacy player is
kept only for backward compatibility with existing v1 drives).

## Technical Specifications

|                  | v2 (CIN2)                         | v1 (legacy)          |
|------------------|------------------------------------|-----------------------|
| Resolution       | 160 x 96                          | 160 x 96               |
| Color depth      | 16 colors (one shared palette)    | 256 colors per frame  |
| Frame rate       | up to 24fps (any rational rate)   | ~10-11fps (uncapped)  |
| Bytes/frame      | 7,680 (15 sectors)                 | 15,872 (31 sectors)   |
| Required throughput @ target fps | ~180 KiB/s @ 24fps    | ~155-170 KiB/s @ 10-11fps |

## Performance

Two build/runtime changes target playback speed directly, both reasoned
from the eZ80 core's known instruction-set limitations (no barrel
shifter, no native 32-bit multiply) rather than measured on hardware:

- The build now compiles at `-O3` (the CE Toolchain default is `-Oz`,
  optimize for *size*). Free performance for a few extra KB of flash.
- `src/decode.c`'s packed-4-bit-to-2x-scaled blit -- the routine that
  runs on every displayed frame -- replaced nibble-shift arithmetic
  with a precomputed lookup table, replaced recomputing the bottom of
  each 2x-scaled row with copying the top row it duplicates, and
  replaced a per-row multiply with a running pointer. Verified
  byte-identical output against an independent reference
  (`tests/test_decode_cross_check.py`), not just "still passes".

Exit (or the live overlay, or Mode to pin it open) reports the
player's own measured average decode time per frame and the FPS that
implies as a ceiling *for the decoder alone* -- e.g. "decode avg: 12.500
ms (decode ceiling: ~80 fps)" means the blit itself isn't what's
capping playback if the observed FPS is much lower than that ceiling;
something else (USB read throughput, `gfx_Wait()`/LCD timing) is. That
split is what makes further optimization work targeted instead of
guesswork -- but it has not yet been measured on physical hardware,
so no FPS number is claimed here as achieved.

## Known limitations

- v2 has been validated with host-side unit/simulation tests (see
  `tests/`) and structural compilation against the real CE-Programming
  toolchain headers, but **not yet on physical TI-84 Plus CE hardware**
  -- there is no ez80 CE toolchain or calculator available in this
  development environment. Build with the real toolchain and soak-test
  before relying on it for anything long-form. See
  `docs/CIN2_FORMAT.md` for the full design rationale and open risks
  (sustained USB throughput, decode+scale CPU budget, hardware read
  latency spikes).
- The v2 encoder targets 160x96 only, matching the calculator-side
  decoder; both would need to change together to support another
  resolution.
- The FAT32 file browser only looks in the drive's root folder, and only
  reads short (8.3) filenames -- long filenames still show up (FAT32
  always stores a short name alongside a long one) but any name-mangling
  applied by the OS that formatted the drive is what you'll see on the
  calculator. Movies inside subfolders aren't listed. Up to 32 playable
  files and up to 256 cluster-chain extents per movie (i.e. a very
  fragmented file on a nearly-full drive) are supported; anything beyond
  those limits is reported as an error rather than silently truncated.
- FAT32 only -- FAT16, exFAT, and NTFS drives are detected as "a
  filesystem Cinema can't read" and rejected with an on-screen message
  rather than misread as movie data. Small/older USB drives (well under
  ~1GB) are sometimes formatted FAT16 by default even when the box says
  "FAT32" -- if you see that message, reformat the drive as FAT32
  explicitly (on Windows: right-click the drive -> Format -> File
  system: FAT32; if FAT32 isn't offered for a very small drive, use
  `format X: /FS:FAT32` from an elevated Command Prompt, or a tool like
  Rufus).

## Development / tests

`tests/run_tests.sh` runs everything that can be validated without real
calculator hardware: `src/decode.c`/`src/cin2.c` unit tests, structural
compilation of all calculator-side sources against transcribed CE
toolchain headers, full player state-machine simulations (prefill,
async slot handling, scheduling, pause, resume, error paths) against a
synthetic in-memory drive, and the Python encoder's tests (`pip install
pillow pytest numpy` first).

## Links

- [FBin GitHub Repository](https://github.com/will-dabeast09/fbin) (v1 encoder)
- [HDD Raw Copy Tool](https://hddguru.com/software/HDD-Raw-Copy-Tool/)
