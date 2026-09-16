# Cinema v0.9.0 Beta 1

Cinema is a USB video player for the TI-84 Plus CE. It plays videos converted to the CIN2 format from compatible FAT32 USB storage.

Beta 1 supports:

- RAW8 and Packed4 CIN2 movies
- FAT32 folders and paged browsing
- Pause and resume
- Forward and backward seeking
- Frame stepping while paused
- Whole-movie looping
- Saved resume positions
- Matching CSU subtitles
- Subtitle delay, style, spacing, and position
- Fast subtitle startup
- Playback and subtitle diagnostics

> Cinema is beta software. The current build has been tested with a specific TI-84 Plus CE, USB drive, filesystem layout, movie, and subtitle setup. Compatibility and performance may vary with other hardware and files.

## Requirements

### Calculator and USB hardware

- TI-84 Plus CE
- Compatible USB connection setup
- FAT32 USB storage
- 512-byte logical sectors
- Sufficient storage for converted movies
- Required CE runtime libraries

The exact cable, adapter, hub, or power arrangement may depend on the USB drive being used.

Cinema does not support exFAT, NTFS, or FAT16.

Back up important files before formatting or modifying a USB drive.

### Computer software

Movie conversion requires:

- Python 3
- Pillow
- FFmpeg available on `PATH`

Install Pillow:

```powershell
py -m pip install pillow
```

Confirm that FFmpeg is available:

```powershell
ffmpeg -version
```

## Install Cinema

1. Transfer `Cinema_Beta_1_Diagnostic.8xp` to the calculator.
2. Prepare a FAT32 USB drive.
3. Convert a video to CIN2.
4. Verify the converted movie.
5. Copy the movie to the USB drive as an ordinary file.
6. Optionally create and copy a matching CSU subtitle file.
7. Run Cinema and connect the USB drive.

Cinema recognizes `.BIN` and `.CIN` movie files.

Short, simple filenames are recommended.

## Convert a Movie

View all current encoder options:

```powershell
py encode_cin2.py --help
```

Replace the example paths below with paths to real files on the computer.

Basic conversion:

```powershell
py encode_cin2.py `
    "C:\Users\YourName\Videos\Input.mp4" `
    "C:\Users\YourName\Videos\MOVIE.BIN"
```

Encode at 18 FPS:

```powershell
py encode_cin2.py `
    "C:\Users\YourName\Videos\Input.mp4" `
    "C:\Users\YourName\Videos\MOVIE.BIN" `
    --fps 18
```

The encoder also supports:

- Rational frame rates
- Batch-folder encoding
- Start and duration limits
- Automatic or manual palette sampling
- Clean or legacy dithering
- Worker-process selection
- Optional SRT subtitle conversion

The encoder creates 160 x 96 video using a shared 16-color palette.

Do not literally enter example paths such as `C:\Path\To\Input.mp4`. Those are placeholders and do not refer to real files.

## Create a Movie with Subtitles

The easiest subtitle workflow is to let the encoder create a matching CSU file:

```powershell
py encode_cin2.py `
    "C:\Users\YourName\Videos\Input.mp4" `
    "C:\Users\YourName\Videos\MOVIE.BIN" `
    --fps 18 `
    --subtitles "C:\Users\YourName\Videos\Input.srt"
```

This creates files similar to:

```text
MOVIE.BIN
MOVIE.CSU
```

Copy both files into the same folder on the FAT32 USB drive.

The movie and CSU must have the same base filename.

## Standalone Subtitle Creation

Cinema also includes a standalone SRT-to-CSU tool.

View its exact required arguments:

```powershell
py srt_to_csu.py --help
```

The current standalone tool requires:

- SRT input path
- CSU output path
- Movie name
- Movie frame count
- Movie FPS numerator
- Movie FPS denominator
- Final movie file size

Because the CSU is associated with the final movie, finish encoding the movie before creating the standalone CSU.

## Verify a Movie

Verify an ordinary CIN2 file before copying it to the USB drive:

```powershell
py verify_cin2.py `
    "C:\Users\YourName\Videos\MOVIE.BIN"
```

A successful structural verification does not guarantee that every USB drive can sustain the selected frame rate.

Known Beta 1 verifier limitation: providing a nonexistent file path may produce a Python traceback instead of a clean missing-file message.

## Verify Subtitles

```powershell
py verify_csu.py `
    "C:\Users\YourName\Videos\MOVIE.CSU"
```

Always verify the CSU generated for the exact final movie.

## Packed4 Movies

Cinema Beta 1 supports Packed4 movies.

Packed4 stores each 160 x 96 frame in 15 sectors and allows paired frame reads when the movie layout permits them. Packed4 is recommended for practical USB playback on the tested setup.

Use the current Packed4 preparation tool included with the project, then verify the resulting movie before hardware testing.

For normal Cinema use, copy the final movie to the FAT32 USB drive as an ordinary file.

Do not overwrite an entire USB drive with a raw movie image unless following a separate, specifically documented raw-image test procedure.

## Recommended USB Layout

Movies may be stored in the root directory or in folders:

```text
USB drive
├── MOVIE.BIN
├── MOVIE.CSU
├── ANOTHER.BIN
└── Movies
    ├── MOVIE2.BIN
    └── MOVIE2.CSU
```

Keep each CSU beside its matching movie.

## Movie Browser Controls

- `Up` / `Down`: move the selection
- `Enter` or `2nd`: play a movie or open a folder
- `Clear`: return to the parent folder or leave Cinema from the root
- `Mode`: open Cinema settings
- On-screen previous and next controls: change browser pages

The browser can display format information, duration, and thumbnails when available.

## Playback Controls

- `2nd` or `Enter`: pause or resume
- `Left` / `Right`: seek backward or forward
- `Down` / `Up`: make larger backward or forward seeks
- `Window`: step one frame forward while paused
- `Y=` while paused: step one frame backward
- `Y=` while playing: toggle subtitles
- `0`: restart from the beginning
- `Graph`: toggle whole-movie looping
- `Mode`: pin or unpin playback status
- `Del`: open Subtitle Options
- `Clear`: exit playback

When resume storage is enabled, Cinema saves the last frame that was actually displayed.

## Subtitle Options

Press `Del` during playback.

- `Up` / `Down`: select an option
- `Left` / `Right`: change the selected option
- `0`: reset subtitle options
- `Clear` or `Del`: return to playback

Subtitle Options include:

- Subtitles enabled or disabled
- Subtitle delay
- Subtitle style
- `Spacing: NORMAL`
- `Spacing: TIGHT`
- Top or bottom placement

Beta 1 includes fast subtitle opening and clean restoration of playback after leaving Subtitle Options.

## Supported Media

- CIN2 movie format
- RAW8 and Packed4 frame storage
- 160 x 96 encoded video
- 320 x 192 displayed video
- Shared 16-color RGB1555 palette
- Rational frame rates
- `.BIN` and `.CIN` movie extensions
- Matching `.CSU` subtitle sidecars
- FAT32 folders and paged browsing
- Legacy v1 playback for compatible older media

Cinema does not play MP4, MKV, or other desktop video formats directly. Convert videos to CIN2 first.

Cinema Beta 1 does not include audio playback.

## Troubleshooting

### The USB drive is not recognized

Confirm that the drive uses FAT32 and 512-byte logical sectors.

Try another compatible USB drive or connection arrangement if initialization fails.

### No movies appear

Confirm that:

- The drive is FAT32.
- The movie uses `.BIN` or `.CIN`.
- The movie is in the current folder.
- Another browser page does not contain the movie.
- The filename is short and simple.

### Subtitles do not appear

Confirm that:

- The CSU and movie use the same base filename.
- Both files are in the same USB folder.
- The CSU was generated for the exact final movie.
- The CSU passes `verify_csu.py`.
- Subtitles are enabled.
- Subtitle delay is near zero.

### Playback is slower than the encoded frame rate

The USB drive may not sustain the required transfer rate.

Try:

- A lower encoder frame rate
- A Packed4 movie
- Another compatible USB drive
- A less fragmented copy of the movie

Cinema favors sequential presentation rather than silently discarding most frames.

### A movie takes time to open

Cinema may need to map the movie's FAT32 allocation.

Opening time can depend on:

- Movie size
- File fragmentation
- Cached placement information
- USB-drive performance
- Filesystem layout

### The verifier says a file does not exist

Replace the example path with the real location of the file.

For example:

```powershell
py verify_cin2.py `
    "C:\Users\YourName\Videos\ActualMovie.bin"
```

## Building from Source

Run the complete host-side test suite:

```bash
bash tests/run_tests.sh
```

Build the fixed-ASM diagnostic configuration:

```powershell
make clean
make CINEMA_RENDERER=fixed_asm CINEMA_BUILD=diagnostic
```

The resulting calculator program is:

```text
bin\CINEMA.8xp
```

The host structural test may require its GraphX stub to declare `gfx_BlitScreen()` when testing the current display-buffer repair.

## Beta 1 Validation

The Beta 1 source checkpoint passed:

- CIN2 host tests
- FAT32 host tests
- Placement-map tests
- Fixed-scale renderer tests
- Streamed CSU tests larger than 64 KiB
- Structural linking against CE stubs
- Player v1 simulation
- Player v2 end-to-end simulation
- Fixed-C renderer integration testing
- 63 Python encoder tests
- Fixed-ASM CEdev compilation

Physical TI-84 Plus CE testing confirmed:

- Packed4 playback on the tested setup
- Long-movie playback from FAT32 USB storage
- Subtitle startup that is effectively immediate
- Working subtitle presentation
- Normal subtitle cue transitions
- Clean return from Subtitle Options without stale or blinking menu text

These results apply only to the exact tested calculator, player build, USB setup, movie, and subtitle files.

## Release Artifact

The physically tested Beta 1 diagnostic program is:

```text
Cinema_Beta_1_Diagnostic.8xp
```

Exact identity:

```text
Size: 48,564 bytes
SHA-256:
E76BFD029986B6ACECE5CEB0123870A0E073731F9AD70299B39AC425F11796C2
```

Repeated CEdev builds produced the same file size but different SHA-256 hashes. The attached release artifact is therefore identified by its exact checksum and should not be replaced with an untested rebuild.

## Known Beta 1 Limitations

- This is a diagnostic beta build.
- Diagnostic pages appear after playback.
- Cinema does not include audio playback.
- USB compatibility and performance vary by drive and setup.
- Video is encoded at 160 x 96.
- Deeply fragmented movies may exceed the bounded extent-map capacity.
- The CIN2 verifier's missing-file path may display a Python traceback.
- The verifier may not accept every CPE1-prepared file.
- Verify ordinary CIN2 files before placement preparation.
- Some encoder help wording still refers to an older raw-drive workflow.
- For normal Beta 1 use, copy movies to FAT32 as ordinary files.
- CEdev packaging was not byte-reproducible in the tested environment.

## Reporting a Problem

Include:

- Calculator model
- Calculator OS version
- Cinema Beta version
- USB-drive model and capacity
- USB filesystem and cluster size, if known
- Movie filename
- RAW8 or Packed4
- Encoded frame rate
- Whether subtitles were enabled
- The last action performed before the problem
- Any displayed error or diagnostic values
- Whether the problem happens repeatedly

Photos of the diagnostic screens can be useful.

## Legal Use

Use Cinema only with media that you have the right to convert, use, and distribute.

No movies or subtitle files are included with Cinema.
