# Cinema upgrades: experimental engineering log

**Branch:** `experimental/cinema-upgrades-20261118`, built on top of the
stability/subtitle repair. **Status: not a release; a real TI-84 Plus CE USB
playback qualification is still required.** No original release file was
replaced by this branch. The `release/cinema-beta-1` branch is unchanged.

## Implemented and host-tested

| Area | What changed | What has been verified |
|---|---|---|
| Source aspect ratio | `--fit stretch` (legacy default), `contain` (black bars), `cover` (crop) | FFmpeg pixel placement and full RAW8/Packed4 conversions |
| Palette | Optional RGB1555-aware, green-weighted refinement (`--palette-method weighted`) | Full-resolution sampled-frame error must improve or original palette is retained; verified CIN2 output |
| Dither | Optional spatially fixed Bayer (`--dither stable`) to avoid frame-random flicker | Frame repeatability, legal 0–15 indexes, 1 vs 4 worker bit identity |
| Desktop GUI | `python tools/cinema_gui.py` | Actual Linux Tk window, 60-frame GUI conversion, independent CIN2 verifier |
| Playback speed | `Zoom` slower, `Trace` faster: 50, 75, 100, 125, 150, 200% | End-to-end player key-injection simulation, every frame still presented |
| Brightness | `+` / `-` adjust RGB1555 palette level -8..+8 | Entire 32,768-color RGB1555 domain, end-to-end control simulation |
| Contrast | `×` / `÷` adjust palette 50–150% | Neutral identity, clamping, runtime key-injection simulation |
| Performance monitor | New `Performance monitor` setting; continuous FPS, render ms, dropped frames, speed, brightness and contrast overlay | CST1 backward-compatibility tests + simulated OSD string |
| Subtitle cache | Two compact per-cue glyph-mask buffers, 480 bytes instead of ~4.8 KiB sprites | Real player integration: single glyph rasterization across 20 displayed frames, pixel at centered position |
| Subtitle timing | 25 ms adjustment instead of 100 ms; no premature frame-zero cue | C tests at 20 FPS and 24000/1001 FPS |
| Subtitle movie binding | Calculate the same filename+length+FPS CRC as `csu_common.movie_id`, and enforce it on opening | Cross-language CRC vectors, mismatched-movie rejection |
| Subtitle style | `OUTLINE` added to BOX/COMPACT/TEXT; computed from cached glyph masks | CEdev compilation and C player simulation; **physical visual check pending** |
| Renderer candidates | Dual-row write, 4-byte unroll, 16-color LUT variants | All compile into diagnostic .8xp files, eZ80 disassembly inspected |

The previous stable behavior remains opt-in/default-compatible where possible:
**default** encoder parameters still use `stretch`, `median`, `clean`. Their
output hashes were compared against the pre-upgrade commit on the same
4-second/12 FPS sample, and matched exactly in both RAW8 and Packed4.
The unmodified Packed4 assembly implementation is the default when building
with `CINEMA_RENDERER=fixed_asm`.

## Running the GUI

Windows:

```powershell
py -m pip install pillow
py tools/cinema_gui.py
```

Install FFmpeg and make sure `ffmpeg -version` works. Linux and macOS can
run `python3 tools/cinema_gui.py`; install the OS package providing tkinter
if needed. The GUI writes a `.bin` file (and optionally `.csu`); it never
writes directly to a USB device. For a safe default, select **Contain** and
leave Packed4 off unless the calculator is using a `fixed_asm` build.

## Renderer A/B builds

### Recommended: one-install automatic test

**Important memory correction (November 19, 2026):** The first auto-test
binary was mistakenly shipped at only 53,916 bytes **compressed**, while its
uncompressed executable occupied **130,512 bytes**. CEdev v15 documents a
64-KiB runtime code/data limit. This could result in TI-OS `ERROR: MEMORY`
even after a RAM reset, because clearing RAM cannot shrink a linked program.
**Do not install that earlier binary again.**

The corrected auto-test uses `-Oz`, ROM-backed CRT routines, and a small
integer/string formatter instead of the large `printf` formatter. It also
omits *legacy v1 playback only in the experimental auto-test binary*; Beta 1
and normal production builds keep v1 support. The corrected auto-test's
uncompressed program is **63,947 bytes**, with 1,589 bytes of code-budget
headroom; its transferable `.8xp` is **32,643 bytes**. Every auto-test build
now runs `tools/check_ce_memory.py` against its linker map and fails if the
64-KiB budget is exceeded. An ordinary RAM clear is not a remedy for an
oversized executable. Actual hardware launch still needs confirmation.

```sh
make -B CINEMA_RENDERER=fixed_asm CINEMA_AUTO_TEST=on
```

This builds **one** `bin/CINEMA.8xp`. On opening a CIN2 movie, Cinema
automatically benchmarks its *first prefetched movie frame* in the offscreen
buffer, using one untimed warm-up and 16 timed renders per candidate. Packed4 compares the
original eZ80 assembly against `dual`, `unrolled`, and `lut`. RAW8 compares
GraphX against fixed-C and fixed-ASM. Each alternative must yield an identical
hash across the **entire 320×240 framebuffer** before it can be chosen; the
winner must also be at least 7% faster than the current valid winner.
Uncertain/tied timings or any mismatched pixels automatically retain the
original. No benchmark frame is ever shown and movie playback begins **after**
the benchmark, so startup overhead does not count toward reported playback
FPS. Selection occurs afresh for each opened movie and requires no settings.

If a matching `.csu` file is loaded, the same build also compares cached
subtitle glyphs with the legacy GraphX path for two lines of text in each of
the four subtitle styles. It enables caching only if all four styles render
identically **and** cached drawing is at least 7% faster. Any mismatch uses
legacy text rendering automatically. This is a conservative pixel-equivalence
check on a synthetic two-line sample, not proof every real caption or special
character is correct. The automatic test finishes with **one summary screen**
showing timing ticks, `OK`/`DIFF`, selected renderer, subtitle-cache status,
measured playback FPS and dropped frames. One key dismisses the summary.

**Important:** A successful CEdev build and host simulation cannot establish
which candidate really wins on a TI-84 Plus CE, and the checksum cannot prove
LCD color accuracy or catch writes outside the framebuffer. The calculator's
automatic result is the hardware measurement; please share a photo of its
summary screen after playing a representative Packed4 movie *with subtitles*.

### Manual comparisons (optional, not required)

```sh
make -B CINEMA_RENDERER=fixed_asm CINEMA_PACKED4_VARIANT=original CINEMA_BUILD=diagnostic
make -B CINEMA_RENDERER=fixed_asm CINEMA_PACKED4_VARIANT=dual CINEMA_BUILD=diagnostic
make -B CINEMA_RENDERER=fixed_asm CINEMA_PACKED4_VARIANT=unrolled CINEMA_BUILD=diagnostic
make -B CINEMA_RENDERER=fixed_asm CINEMA_PACKED4_VARIANT=lut CINEMA_BUILD=diagnostic
```

All four compile using the real CEdev toolchain. The `lut` variant's lookup
table was found at a 256-byte-aligned address in the linked eZ80 object,
as required by its fast low-byte indexing. These results prove **assembly
and linkage only**, not runtime performance or eZ80 VRAM semantics. Test
each binary with exactly the same media, USB drive and power setup, and
record presented FPS, maximum frame delay, dropped frames and rendering
time on the diagnostic pages. Do not ship a different variant merely on
the basis of code size or host-side cycle estimates.

To A/B test the subtitle optimization independently, add
`CINEMA_SUBTITLE_CACHE=off` to a build for the original GraphX text
rendering path (including a fall-back outlined-text style). The default is
`on`. This is the safest way to pinpoint a physical font/appearance
regression without undoing the rest of the player improvements.

## 32 colors: format and throughput study

`tools/packed5_lab.py` is an intentionally isolated research tool. It
quantizes the same input frames into 16 and 32 RGB1555 palette entries,
measures green-weighted squared color error using the *displayed* colors,
and exhaustively round-trips each 5-bit packed pixel stream. **It does
not write CIN2 v2 files.** The v2 player only supports 16-color palettes.

| Format | Bytes/pixel | Useful bytes/frame | 512-byte USB sectors | Transfer per frame |
|---|---:|---:|---:|---:|
| Packed4 (16 colors) | 0.5 | 7,680 | 15 | 7,680 B |
| Experimental Packed5 (32 colors) | 0.625 | 9,600 | 19 | 9,728 B |

At 24 FPS, Packed5 requires 228 KiB/s; at 30 FPS, 285 KiB/s. Official CEdev
USB mass-storage documentation reports about 262–273 KiB/s on its tested
drives, so **30 FPS Packed5 is not currently a responsible default**.
At 15 FPS it requires 142.5 KiB/s (still requires a decoder measurement).

Measured 16-to-32-color changes on 16 sampled frames (fixed palettes, no
dithering, representative but limited clips):

| Input | 16-color weighted MSE | 32-color weighted MSE | Reduction |
|---|---:|---:|---:|
| Generated `testsrc2` 320x180@24 | 3,015.80 | 2,301.84 | 23.7% |
| Repository `test_drive.gif` | 239.06 | 123.10 | 48.5% |
| Repository `dreamworks.gif` | 71.65 | 43.27 | 39.6% |

MSE measures quantization color error, not viewer preference. A future
valid 32-color format must include new on-disk versioning, a strict
validator, an eZ80 packed5 decoder, a memory-budget check, and physical
frame-rate/USB stress tests before the player accepts it.

## What is NOT finished or validated yet

- **Real calculator ROM emulation**: CEmu's official autotester requires a
  ROM dumped from an owned TI-84 Plus CE; none is available here. CEdev
  linking, host-C USB/player simulation and actual FFmpeg conversion are
  the closest available checks. Do not imply measured hardware FPS.
- **Physical subtitle appearance**: host tests prove placement and reuse of
  bitmaps with a simulated font but cannot prove the real GraphX character
  glyph rendering is identical. Confirm all four styles on hardware.
- **Playback thumbnail scrubbing**: the file browser already shows movie
  thumbnails, but an interactive seek-thumbnail overlay is a separate
  unimplemented feature. It requires a buffer/USB scheduling design that
  does not compete with the active movie read.
- **Multiple subtitle languages, arbitrary custom fonts, cue-aware auto
  positioning and persisted per-movie subtitle preferences**: pending
  backwards-compatible sidecar and settings design.
- **Packed5 real-time playback**: only palette/bitstream analysis exists,
  no new CIN2 variant has been enabled in the calculator player.
- **Windows and macOS GUI**: command generation is platform-independent
  and tested, but only the Linux Tk window was physically opened here.

For any release, complete `docs/HARDWARE_TEST_PLAN.md` again using the new
diagnostic binary and representative short, long, fragmented, packed,
subtitle-heavy, and high-motion movies.
