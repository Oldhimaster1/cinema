# Cinema browser converter

The website at `docs/index.html` converts ordinary videos into Cinema movie files directly in a browser. It is a static website, so it can run on GitHub Pages without a server or a login. Videos stay on the user's computer.

The output is a 160 × 96, 16-color Packed4 `.BIN` movie. Optional `.srt` subtitles become a matching `.CSU` file. Movie names are automatically shortened to eight letters/numbers for the calculator's FAT32 movie browser. The converter does not include audio.

## Set up GitHub Pages

In the repository's **Settings → Pages**, select **Deploy from a branch**, choose the `main` branch and the `/docs` folder, then save. If using the separate `gh-pages` branch instead, choose that branch and `/ (root)`.

The public website will normally appear at <https://oldhimaster1.github.io/cinema/> after Pages finishes deploying.

## Browser support

- Recent desktop Chrome and Edge support modern video decoding and direct saving to a chosen folder. They are recommended for full-length movies.
- Other browsers may support the video decoder but not direct file saving. In that case, conversion is limited to 128 MiB of output so the website does not try to hold an entire movie in memory.
- The video must use a format that the browser's decoder supports, such as MP4 with H.264 video. An unsupported file produces an explanatory message.
- Higher frame rates can produce a larger file and may exceed the calculator or flash drive's real playback speed. 18 FPS is the default.
- For long movies, conversion and saving can take a while; leave the page open.

The website includes a local copy of the [Mediabunny](https://mediabunny.dev/) JavaScript video library (`vendor/mediabunny.min.mjs`, Mozilla Public License 2.0). Its license is included at `vendor/LICENSE`. No external script host is needed for conversion.

## Validation

Run the web format tests with:

```bash
node tests/test_web_codec.mjs
python -m pytest -q tests/test_web_codec.py
```

The Node test generates real movie and subtitle file bytes. The Python test checks those files with the independent movie and subtitle verifiers. This checks file compatibility, not the video decoder or the real speed of playback on the calculator. The full suite is `bash tests/run_tests.sh`.
