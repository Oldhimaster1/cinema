# Cinema hardware acceptance checklist

The host test suite and CEdev compiler cannot measure real USB transfer times,
eZ80 rendering cost, subtitle visibility, or frame pacing on a TI-84 Plus CE.
Keep the physically tested `Cinema_Beta_1_Diagnostic.8xp` available as a
rollback. **Do not overwrite the published release program with a newly built
binary until the checks below pass on the intended hardware.**

## Reproducible setup

Record the calculator model and OS, USB adapter/hub/cable, USB drive model,
FAT32 cluster size, and whether the drive is fragmented. For an A/B comparison,
use the same calculator, drive, clips, and settings with the original Beta 1
diagnostic program and the new `fixed_asm` diagnostic build. Disable changing
settings between runs. Run each workload at least three times.

Generate a few short `--packed4` clips (15, 24, and optionally 30 FPS), a
matching RAW8 clip, one 10-minute movie, and a longer movie if storage allows.
For the subtitle clip, supply an SRT with a cue at the beginning, two adjacent
cues, a quiet gap, a two-line cue, and a cue ending at the final frame. Include
a `--start` + `--duration` conversion to check timeline alignment. Verify each
result before copying it:

```sh
python3 tools/verify_cin2.py MOVIE.BIN
python3 tools/verify_csu.py MOVIE.CSU
```

Copy `.BIN` and `.CSU` files into the same FAT32 folder with matching base names.
Do not write the BIN as a whole-drive raw image for these tests.

## Playback and diagnostics

| Test | Expected behavior / what to record |
| --- | --- |
| 15/24/30 FPS Packed4 | Record `actual`, `target`, `speed`, `drop`, `wait`, `lag`, `render ms`, USB `KiB/s` and maximum read duration from the diagnostic screens. Compare against the original Beta. |
| RAW8 on GraphX/fixed-C | Displays the same frame colors; Packed4 on these builds must report that fixed-ASM is required, before reading frames. |
| Subtitle timeline | First cue appears at the intended frame, middle cues switch without ghosting, quiet gaps are blank, and last cue disappears. Repeat after pause/resume/seek/restart. |
| Subtitle options | Toggle via `Y=`; change position, spacing and delay; open/close options with `Del`. No stale pixels or rapid blinking. |
| Repeated playback | Replay/loop a short clip at least ten times; no crash, stuck read, or visibly growing startup delay. |
| Long playback | At least ten minutes continuously, and preferably an entire long movie with subtitles enabled. Record time to any stall or USB error. |
| Resume and seek | Seek forward/backward, frame-step paused video, save resume and reopen; displayed frame number remains correct. |
| FAT32 browser/cache | First and second open of the same movie, folders/pages, and opening a *replaced* movie of the same filename. Check new frames are displayed. |
| Failures and recovery | Missing CSU, damaged CSU, damaged CIN2 header, and USB disconnect should produce recoverable behavior; no memory corruption or hang. |

For a 24-FPS target, a recorded `actual` around 24 FPS and no persistent
buffering is a good result. Consistently below the target, or worse `lag` and
`wait` values than the original Beta on the same setup, is a regression and
blocks replacing the release. A three-run median is more useful than a single
startup measurement. Do not assume that a fast USB read alone guarantees fast
rendering; both operations compete for calculator time.

The expected *payload* bandwidth at 24 FPS is ~180 KiB/s for Packed4 and
~360 KiB/s for RAW8, before filesystem/USB command overhead. Official CEdev
MSD documentation reports around 262-273 KiB/s reads for its tested drives;
other drives can differ. Packed4 saves bandwidth but actual FPS must be
measured on the calculator.

## Acceptance decision

Consider a new binary release-ready only if it passes all relevant playback
and subtitle tests above, reaches the same or better *sustained* FPS and
stability than the original Beta on identical hardware, and shows no new
format/read errors. Save photos of both diagnostic pages and of subtitle
transitions with the exact build hash, so a later regression is reproducible.

If any test fails, keep using the original Beta executable, record the
smallest clip and USB layout that reproduce the failure, and fix/test that
case before trying another release build.
