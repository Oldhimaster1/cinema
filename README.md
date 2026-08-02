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

1. **Prepare your USB drive** -- unformat and wipe it completely.
2. **Encode your video:**
   ```
   pip install pillow
   python3 tools/encode_cin2.py input.mp4 output.bin --fps 24
   ```
   Requires `ffmpeg` on your PATH. Use `--fps 24000/1001` for
   film-rate content, `--start`/`--duration` to trim, and
   `--palette-samples` to control how many frames are sampled when
   building the movie's global 16-color palette (see `--help`).
3. **Write to thumb drive**, starting at byte 0 / LBA 0, e.g.
   `sudo dd if=output.bin of=/dev/sdX bs=1M conv=fsync` (Linux/macOS) or
   [HDD Raw Copy Tool](https://hddguru.com/software/HDD-Raw-Copy-Tool/)
   (Windows).
4. **Install on calculator** -- transfer `CINEMA.8xp` to your TI-84 Plus CE.

### v1 (legacy) drives

Existing v1 drives produced by [FBin](https://github.com/will-dabeast09/fbin)
still work unmodified -- just write the binary as before. Cinema detects
the format automatically from the drive.

## Usage

1. Run CINEMA on your calculator and insert your prepared USB drive.
2. If you've watched this movie before, choose to resume or restart.
3. Playback begins automatically.
4. **v2:** 2nd pauses/resumes, Clear exits (and saves resume state).
   **v1:** any key exits.

## Technical Specifications

|                  | v2 (CIN2)                         | v1 (legacy)          |
|------------------|------------------------------------|-----------------------|
| Resolution       | 160 x 96                          | 160 x 96               |
| Color depth      | 16 colors (one shared palette)    | 256 colors per frame  |
| Frame rate       | up to 24fps (any rational rate)   | ~10-11fps (uncapped)  |
| Bytes/frame      | 7,680 (15 sectors)                 | 15,872 (31 sectors)   |
| Required throughput @ target fps | ~180 KiB/s @ 24fps    | ~155-170 KiB/s @ 10-11fps |

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

### Cinema v2 player controls

- **2nd:** Play or pause
- **Left/Right:** Seek using the configured interval
- **Mode:** Show or hide the playback dashboard
- **Del:** Open the player menu
- **Clear:** Exit and save the resume position

The player menu provides movie information, chapters, eight persistent bookmarks, scaling modes, settings, restart, and return to playback. Optional CRC-protected C2MD metadata stores a title and up to twelve chapters without moving frame data from LBA 1.
