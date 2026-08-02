# Cinema v2 feature candidate validation

Implemented requested player features:

1. Playback dashboard
2. Current/total time and progress bar
3. Configurable backward/forward seeking
8. Playback information screen
11. Optional movie title metadata
12. Up to twelve chapters
13. Eight persistent user bookmarks
14. Exact 2x, original-size, and stretch scaling
19. Four-slot buffering indicator
20. Persistent settings menu

## Controls

- 2nd: pause/resume
- Left/Right: seek by configured interval
- Mode: dashboard
- Del: player menu
- Clear: exit/save
- Menu Up/Down: selection
- Menu 2nd: activate

## Validation completed

- Strict C99 compilation with `-Wall -Wextra -Wpedantic -Werror`
- Original CIN2 decoder/parser tests
- Original v1 and v2 state-machine simulations
- New metadata CRC/title/chapter tests
- New original-size and stretch decoder tests
- New compact-font/dashboard tests
- New settings persistence tests
- New bookmark persistence tests
- UndefinedBehaviorSanitizer runs
- 24-hour scheduler arithmetic model at 24/1 and 24000/1001
- Metadata patcher syntax and CRC test

## Evidence boundary

The authentic CEdev Toolchain was unavailable (`cedev-config` missing), so no genuine `.8xp` was produced. Host tests cannot establish eZ80 performance, real GraphX/LCD behavior, physical key handling, USB latency, or hardware stability. This source must be built with CEdev and tested first in CEmu and then on a physical TI-84 Plus CE.
