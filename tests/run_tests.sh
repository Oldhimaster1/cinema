#!/usr/bin/env bash
# Runs every host-side test for Cinema. There is no ez80 CE toolchain
# available in most dev environments, so these tests validate what can
# be validated on a normal machine:
#
#   1. Pure logic (src/cin2.c) compiled and unit-tested directly with
#      the host compiler -- deliberately free of calculator-specific
#      headers for exactly this reason.
#   2. Structural validation of the calculator-only sources (main.c,
#      player_v1.c, player_v2.c, msd_util.c) by compiling and linking
#      them against hand-written stub headers/implementations of
#      graphx.h/msddrvce.h/usbdrvce.h/fileioc.h/tice.h (tests/stub_*),
#      transcribed from the real CE-Programming/toolchain headers. This
#      catches undeclared identifiers, wrong signatures, type
#      mismatches, and missing includes that a plain code review can
#      miss.
#   3. End-to-end simulation of both players' control flow (prefill,
#      async slot state machine, scheduler, resume save/load, pause,
#      error handling) against a synthetic in-memory "drive", so real
#      logic executes -- not just links.
#   4. The Python encoder (tools/encode_cin2.py) and its round-trip
#      correctness against src/cin2.c's on-disk format.
#
# None of this replaces building and testing on real hardware -- see
# docs/CIN2_FORMAT.md and the README's "Known limitations" section.
set -euo pipefail
cd "$(dirname "$0")/.."

CC=${CC:-gcc}
CFLAGS="-Wall -Wextra -Wpedantic -std=c99"
STUB_CFLAGS="-Wall -Wextra -std=c99 -Itests/stub_include -Isrc"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass() { printf '\033[32mPASS\033[0m %s\n' "$1"; }
run_step() {
    local name=$1; shift
    echo "--- $name ---"
    "$@"
    pass "$name"
}

test_cin2() {
    $CC $CFLAGS -o "$TMP/test_cin2" tests/test_cin2.c src/cin2.c
    "$TMP/test_cin2"
}

test_structural_link() {
    $CC $STUB_CFLAGS \
        src/main.c src/player_v1.c src/player_v2.c src/msd_util.c src/cin2.c \
        src/fat32ro.c src/render_v2.c \
        tests/stub_impl.c -o "$TMP/cinema_stub_link"
    "$TMP/cinema_stub_link"
}

test_player_v2_sim() {
    $CC $STUB_CFLAGS -Wl,--wrap=clock \
        src/player_v2.c src/cin2.c src/msd_util.c src/fat32ro.c src/render_v2.c \
        tests/stub_impl_sim.c tests/test_player_v2_sim.c -o "$TMP/test_player_v2_sim"
    timeout 30 "$TMP/test_player_v2_sim"
}

# Focused integration check, not the whole test_player_v2_sim.c suite:
# with CINEMA_RENDERER_FIXED_C selected instead of the default (mocked)
# GraphX call, confirms real playback through player_v2_loop() actually
# calls render_scaled_fixed_c() and produces correct on-screen pixels --
# not just that the isolated algorithm is correct (test_render_v2.c
# already proves that) or that playback "doesn't crash".
test_player_v2_sim_fixed_c_integration() {
    $CC $STUB_CFLAGS -Wl,--wrap=clock -DCINEMA_RENDERER=CINEMA_RENDERER_FIXED_C \
        src/player_v2.c src/cin2.c src/msd_util.c src/fat32ro.c src/render_v2.c \
        tests/stub_impl_sim.c tests/test_player_v2_sim_fixed_c_integration.c \
        -o "$TMP/test_player_v2_sim_fixed_c_integration"
    timeout 30 "$TMP/test_player_v2_sim_fixed_c_integration"
}

test_fat32ro() {
    $CC $CFLAGS -o "$TMP/test_fat32ro" tests/test_fat32ro.c src/fat32ro.c
    "$TMP/test_fat32ro"
}

test_render_v2() {
    $CC $STUB_CFLAGS -o "$TMP/test_render_v2" tests/test_render_v2.c src/render_v2.c
    "$TMP/test_render_v2"
}

test_player_v1_sim() {
    $CC $STUB_CFLAGS \
        src/player_v1.c src/msd_util.c \
        tests/stub_impl_v1_sim.c tests/test_player_v1_sim.c -o "$TMP/test_player_v1_sim"
    timeout 30 "$TMP/test_player_v1_sim"
}

test_encoder() {
    if ! command -v python3 >/dev/null; then
        echo "python3 not found, skipping encoder tests" >&2
        return 0
    fi
    python3 -m pytest -q tests/test_cin2_format.py tests/test_encode_cin2.py \
        tests/test_fixtures.py tests/test_frame_rate_boundaries.py \
        tests/test_fuzz_cin2_header.py
}

run_step "cin2.c host unit tests"                   test_cin2
run_step "fat32ro.c host unit tests"                test_fat32ro
run_step "render_v2.c fixed-scale pixel correctness" test_render_v2
run_step "structural link against stub CE headers"  test_structural_link
run_step "player_v2 end-to-end simulation"           test_player_v2_sim
run_step "player_v2 sim w/ fixed-C renderer"         test_player_v2_sim_fixed_c_integration
run_step "player_v1 end-to-end simulation"           test_player_v1_sim
run_step "Python encoder tests"                      test_encoder

echo
echo "All host-side tests passed."
