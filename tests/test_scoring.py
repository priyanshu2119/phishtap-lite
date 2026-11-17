"""
Scoring and round timing.

The formula is fixed by docs/ARCHITECTURE.md §6, so these tests assert the exact
documented range and the exact boundary behaviour. A scoring change that breaks
one of these is a spec change and should be made deliberately, not accidentally.
"""

from __future__ import annotations

import pytest

from phishtap.scoring import round_limit_ms, score_answer


# ── The documented range ──────────────────────────────────────────────────

def test_minimum_score_is_ten():
    """A correct answer with no time left and no streak scores exactly 10."""
    r = score_answer(correct=True, remaining_ms=0, limit_ms=14000, streak_before=0)
    assert r.points == 10
    assert r.speed == 0
    assert r.multiplier == 1.0


def test_maximum_score_is_fifty_four():
    """An instant answer at a streak of 8 or more scores exactly 54."""
    r = score_answer(correct=True, remaining_ms=14000, limit_ms=14000, streak_before=8)
    assert r.points == 54
    assert r.speed == 8
    assert r.multiplier == 3.0


def test_multiplier_caps_at_three():
    """A streak beyond 8 does not keep raising the multiplier."""
    at_8 = score_answer(correct=True, remaining_ms=14000, limit_ms=14000, streak_before=8)
    at_50 = score_answer(correct=True, remaining_ms=14000, limit_ms=14000, streak_before=50)
    assert at_8.multiplier == 3.0
    assert at_50.multiplier == 3.0
    assert at_8.points == at_50.points == 54


def test_wrong_answer_scores_zero_regardless_of_speed():
    r = score_answer(correct=False, remaining_ms=14000, limit_ms=14000, streak_before=20)
    assert r.points == 0
    assert r.speed == 0
    assert r.multiplier == 0.0
    assert r.correct is False


# ── Speed tiers ───────────────────────────────────────────────────────────

@pytest.mark.parametrize(
    "remaining,limit,expected_speed",
    [
        (14000, 14000, 8),   # instant
        (12250, 14000, 7),   # 87.5% -> floor(7.0)
        (7000, 14000, 4),    # exactly half
        (1750, 14000, 1),    # 12.5%
        (1749, 14000, 0),    # just under 12.5%
        (0, 14000, 0),       # no time left
    ],
)
def test_speed_is_floor_of_eight_times_fraction(remaining, limit, expected_speed):
    r = score_answer(correct=True, remaining_ms=remaining, limit_ms=limit, streak_before=0)
    assert r.speed == expected_speed


@pytest.mark.parametrize(
    "streak,expected_mult",
    [(0, 1.0), (1, 1.25), (2, 1.5), (4, 2.0), (7, 2.75), (8, 3.0)],
)
def test_multiplier_is_quarter_per_streak(streak, expected_mult):
    r = score_answer(correct=True, remaining_ms=7000, limit_ms=14000, streak_before=streak)
    assert r.multiplier == pytest.approx(expected_mult)


def test_score_is_monotonic_in_remaining_time():
    """Answering faster must never score less. A non-monotonic curve would let a
    player gain by hesitating, which is the opposite of what the game teaches."""
    scores = [
        score_answer(correct=True, remaining_ms=ms, limit_ms=14000, streak_before=0).points
        for ms in range(0, 14001, 500)
    ]
    assert scores == sorted(scores), f"score decreased as time remaining grew: {scores}"


def test_score_is_monotonic_in_streak():
    scores = [
        score_answer(correct=True, remaining_ms=7000, limit_ms=14000, streak_before=s).points
        for s in range(0, 15)
    ]
    assert scores == sorted(scores)


# ── Hostile inputs ────────────────────────────────────────────────────────

def test_remaining_above_limit_is_clamped():
    """A late render ack or a clock glitch could report more time than the round
    had. Clamping to 1 keeps the score at the maximum rather than exceeding it."""
    r = score_answer(correct=True, remaining_ms=99999, limit_ms=14000, streak_before=0)
    assert r.speed == 8
    assert r.points == 18   # (10 + 8) * 1.0


def test_negative_remaining_is_clamped_to_zero():
    r = score_answer(correct=True, remaining_ms=-5000, limit_ms=14000, streak_before=0)
    assert r.speed == 0
    assert r.points == 10
    assert r.remaining_ms == 0


def test_zero_limit_does_not_divide_by_zero():
    """A zero limit would be a bug upstream, but it must not crash the scorer."""
    r = score_answer(correct=True, remaining_ms=0, limit_ms=0, streak_before=0)
    assert r.points == 10


def test_negative_limit_does_not_crash():
    r = score_answer(correct=True, remaining_ms=100, limit_ms=-1, streak_before=0)
    assert r.points >= 10


def test_breakdown_is_frozen():
    """The breakdown is shared with the review payload; it must not be mutable."""
    r = score_answer(correct=True, remaining_ms=7000, limit_ms=14000, streak_before=2)
    with pytest.raises(Exception):
        r.points = 999  # type: ignore[misc]


# ── Round timing ──────────────────────────────────────────────────────────

def test_first_round_is_fourteen_seconds():
    assert round_limit_ms(0) == 14000


def test_round_time_shrinks_by_eight_hundred_ms():
    assert round_limit_ms(1) == 13200
    assert round_limit_ms(5) == 10000
    assert round_limit_ms(10) == 6000


def test_round_time_floors_at_four_seconds():
    """14 - 0.8*13 = 3.6, which the floor raises to 4.0."""
    assert round_limit_ms(13) == 4000
    assert round_limit_ms(50) == 4000
    assert round_limit_ms(1000) == 4000


def test_round_limits_stay_inside_the_mcu_bounds():
    """The firmware rejects a limit outside [4000, 14000] ms, so every value this
    function can produce must be inside that range or pt.start_round will fail."""
    from phishtap.config import ROUND_LIMIT_MAX_S, ROUND_LIMIT_MIN_S

    lo, hi = int(ROUND_LIMIT_MIN_S * 1000), int(ROUND_LIMIT_MAX_S * 1000)
    for i in range(0, 200):
        limit = round_limit_ms(i)
        assert lo <= limit <= hi, f"round {i} produced {limit}, outside [{lo}, {hi}]"


def test_negative_round_index_is_clamped():
    assert round_limit_ms(-5) == 14000
