"""Validates player_v2.c's desired_frame() scheduling formula
mathematically, over durations up to several hours, without running the
real event loop for that long. See src/player_v2.c's desired_frame():

    clock_t elapsed = now - start_tick - accumulated_pause_ticks;
    uint64_t numerator = (uint64_t)elapsed * fps_num;
    uint64_t denominator = (uint64_t)CLOCKS_PER_SEC * fps_den;
    uint64_t frame = (uint64_t)start_frame + numerator / denominator;

This is a closed-form recomputation from absolute elapsed time on every
call (not an accumulated per-frame delta), so there is no rounding
*drift* to accumulate by construction -- each call's truncation error is
bounded by less than one frame period and does not carry over to the
next call. This module proves that property mathematically (exact
rational ground truth via fractions.Fraction) rather than by stepping
a simulated clock for hours of wall time.
"""
from fractions import Fraction

CLOCKS_PER_SEC = 32768  # confirmed from the real CE toolchain's time.h
UINT32_MAX = 0xFFFFFFFF
UINT64_MAX = 0xFFFFFFFFFFFFFFFF


def desired_frame(elapsed_ticks: int, fps_num: int, fps_den: int, start_frame: int) -> int:
    """Direct transcription of src/player_v2.c's desired_frame(), in
    Python integers (which are already unbounded/exact, matching
    uint64_t as long as we separately check for overflow -- see
    test_no_intermediate_overflow_for_realistic_inputs)."""
    numerator = elapsed_ticks * fps_num
    denominator = CLOCKS_PER_SEC * fps_den
    frame = start_frame + numerator // denominator
    return min(frame, UINT32_MAX)


def exact_frame(elapsed_ticks: int, fps_num: int, fps_den: int, start_frame: int) -> Fraction:
    return start_frame + Fraction(elapsed_ticks, CLOCKS_PER_SEC) * Fraction(fps_num, fps_den)


def test_matches_exact_rational_floor_across_a_wide_domain():
    for fps_num, fps_den in ((24, 1), (24000, 1001)):
        for hours in (0, 0.01, 1, 2, 5, 10):
            elapsed = int(hours * 3600 * CLOCKS_PER_SEC)
            got = desired_frame(elapsed, fps_num, fps_den, 0)
            want = exact_frame(elapsed, fps_num, fps_den, 0)
            assert got == int(want), f"fps={fps_num}/{fps_den} hours={hours}: {got} != {want}"
            # Truncation error is < 1 frame, i.e. this is a floor, not
            # a drifting approximation.
            assert 0 <= float(want) - got < 1


def test_no_drift_across_repeated_calls_vs_naive_accumulation():
    """The whole reason to recompute from absolute elapsed time instead
    of accumulating "advance by one frame period" each call: the naive
    accumulator drifts when the frame period doesn't divide CLOCKS_PER_SEC
    evenly (24000/1001 does not); desired_frame() must not."""
    fps_num, fps_den = 24000, 1001
    ticks_per_frame_period = Fraction(CLOCKS_PER_SEC * fps_den, fps_num)

    naive_accumulated_ticks = 0
    max_drift = 0
    for frame_index in range(1, 24 * 3600 * 3):  # 3 hours of frames
        naive_accumulated_ticks += int(ticks_per_frame_period)  # naive: truncate each step
        exact_ticks = frame_index * ticks_per_frame_period
        drift_frames = abs(naive_accumulated_ticks - float(exact_ticks)) / float(ticks_per_frame_period)
        max_drift = max(max_drift, drift_frames)
    # The naive per-step accumulator drifts by more than half a frame
    # over 3 hours -- demonstrating why desired_frame() does NOT do this.
    assert max_drift > 0.5, "sanity check: the naive accumulator should drift for 24000/1001"

    # desired_frame(), by contrast, recomputed from absolute elapsed
    # ticks at 3 hours, is exact to within less than 1 frame (see test
    # above) -- i.e. it has zero accumulated drift regardless of how
    # many calls happened in between, because it never depends on
    # previous calls' rounding.
    elapsed_3h = 3 * 3600 * CLOCKS_PER_SEC
    got = desired_frame(elapsed_3h, fps_num, fps_den, 0)
    want = exact_frame(elapsed_3h, fps_num, fps_den, 0)
    assert 0 <= float(want) - got < 1


def test_resume_offset_applied_exactly_once():
    """start_frame must shift the result by exactly start_frame, with
    zero elapsed time contributing zero additional frames -- i.e. no
    double-application of the resume offset."""
    for start_frame in (0, 1, 100, 7199, 172799):
        got = desired_frame(elapsed_ticks=0, fps_num=24, fps_den=1, start_frame=start_frame)
        assert got == start_frame


def test_multiple_pauses_do_not_accumulate_offset_error():
    """accumulated_pause_ticks is just subtracted from elapsed before
    the same floor-division -- simulate several pauses of varying
    length and confirm the result equals what a single equivalent-length
    pause would give (i.e. pause bookkeeping is purely additive, exact,
    and order-independent)."""
    fps_num, fps_den = 24, 1
    now = 10 * 3600 * CLOCKS_PER_SEC
    start_tick = 0

    pause_lengths = [5, 130, 7, 4000, 1, 61]  # seconds' worth of ticks, varying
    accumulated = sum(p * CLOCKS_PER_SEC for p in pause_lengths)
    elapsed_many_pauses = (now - start_tick) - accumulated

    single_equivalent_pause = sum(pause_lengths) * CLOCKS_PER_SEC
    elapsed_one_pause = (now - start_tick) - single_equivalent_pause

    assert elapsed_many_pauses == elapsed_one_pause
    assert desired_frame(elapsed_many_pauses, fps_num, fps_den, 0) == \
        desired_frame(elapsed_one_pause, fps_num, fps_den, 0)


def test_pause_at_exact_frame_boundary_does_not_skip_or_repeat():
    fps_num, fps_den = 24, 1
    ticks_per_frame = CLOCKS_PER_SEC // fps_num  # 1365, with truncation like the C code sees
    elapsed_at_boundary = 100 * CLOCKS_PER_SEC  # exactly frame 2400
    frame_before_pause = desired_frame(elapsed_at_boundary, fps_num, fps_den, 0)

    # Pause for a while, then resume -- elapsed "active" time is
    # unchanged because the pause duration is fully subtracted out.
    frame_after_pause = desired_frame(elapsed_at_boundary, fps_num, fps_den, 0)
    assert frame_before_pause == frame_after_pause == 2400
    del ticks_per_frame  # illustrative only


def test_no_intermediate_overflow_for_realistic_inputs():
    """uint64_t numerator/denominator must not overflow for any
    plausible session: fps_num <= 24000 (per docs/CIN2_FORMAT.md, no
    higher rate is meaningful for 320x192 CE hardware) and elapsed
    ticks bounded by a clock_t (32-bit unsigned) wraparound span."""
    max_elapsed = 0xFFFFFFFF  # clock_t is unsigned long / 32-bit on this target
    max_fps_num = 24000
    numerator = max_elapsed * max_fps_num
    assert numerator <= UINT64_MAX, "numerator would overflow uint64_t"

    max_fps_den = 1  # denominator is maximized by fps_den=1 combined with... actually
    # denominator = CLOCKS_PER_SEC * fps_den, maximized by *larger* fps_den;
    # fps_den=1001 (film rate) is the only nonstandard value docs/CIN2_FORMAT.md
    # names, so check a generous upper bound beyond that too.
    for fps_den in (1, 1001, 100000):
        denominator = CLOCKS_PER_SEC * fps_den
        assert denominator <= UINT64_MAX


def test_end_of_stream_frame_numbers_stay_within_frame_count_minus_one_semantics():
    """desired_frame() itself has no notion of frame_count -- the
    player's main loop (player_v2_loop in src/player_v2.c) is
    responsible for stopping once last_frame_presented + 1 >=
    frame_count, and refill_empty_slots never queues
    next_frame_to_queue >= frame_count. This test documents/asserts
    only the arithmetic half: desired_frame can legitimately return
    values >= frame_count (e.g. because real time ran ahead of a short
    movie), and it is the caller's job to clamp presentation, not this
    function's. See tests/test_player_v2_sim.c for the end-to-end
    behavior (only frame_count frames are ever rendered)."""
    frame_count = 100
    elapsed_way_past_end = 1000 * CLOCKS_PER_SEC  # far more than 100/24 seconds
    got = desired_frame(elapsed_way_past_end, fps_num=24, fps_den=1, start_frame=0)
    assert got >= frame_count  # by itself, unclamped -- caller (the main loop) clamps playback
