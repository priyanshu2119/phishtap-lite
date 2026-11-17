"""
PhishTap Lite — scoring.

Implements the survival-mode scoring from docs/ARCHITECTURE.md §6:

    frac  = clamp(remaining_ms / limit_ms, 0, 1)
    speed = floor(8 * frac)
    mult  = min(3.0, 1.0 + 0.25 * streak_before)
    pts   = floor((10 + speed) * mult)

Range: 10 points for a slow correct answer with no streak, up to 54 for an
instant answer at a streak of 8 or more. Wrong or timeout scores zero.

The streak is the run of consecutive correct answers BEFORE this one, reset to
zero by any wrong answer or timeout. Passing ``streak_before`` rather than
reading it from state keeps this function pure and testable.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class ScoreBreakdown:
    """The components of a single answer's score, for the review page.

    Frozen so it can be hashed and safely shared between threads without
    accidental mutation.
    """

    correct: bool
    remaining_ms: int
    limit_ms: int
    streak_before: int
    speed: int
    multiplier: float
    points: int


def _clamp(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def score_answer(
    *,
    correct: bool,
    remaining_ms: int,
    limit_ms: int,
    streak_before: int,
) -> ScoreBreakdown:
    """Score one answer.

    ``remaining_ms`` is clamped to ``[0, limit_ms]`` so a late ack or a clock
    glitch cannot produce a fraction above 1 or below 0. ``limit_ms`` must be
    positive; a zero or negative limit is treated as "no time left", which
    scores the minimum for a correct answer and zero otherwise.
    """
    if not correct:
        return ScoreBreakdown(
            correct=False,
            remaining_ms=max(0, remaining_ms),
            limit_ms=max(0, limit_ms),
            streak_before=streak_before,
            speed=0,
            multiplier=0.0,
            points=0,
        )

    safe_limit = limit_ms if limit_ms > 0 else 1
    frac = _clamp(remaining_ms / safe_limit, 0.0, 1.0)
    speed = int(8 * frac)  # floor: int() truncates toward zero, frac >= 0
    mult = min(3.0, 1.0 + 0.25 * streak_before)
    pts = int((10 + speed) * mult)  # floor

    return ScoreBreakdown(
        correct=True,
        remaining_ms=max(0, remaining_ms),
        limit_ms=max(0, limit_ms),
        streak_before=streak_before,
        speed=speed,
        multiplier=mult,
        points=pts,
    )


def round_limit_ms(round_index: int) -> int:
    """Round time in milliseconds, per the game rules.

    ``max(4, 14 - 0.8 * round_index)`` seconds, floored to 4 s. Round 0 is the
    first round. The MCU validates that the limit it receives is within
    [4000, 14000] ms, so this function must stay inside that range.
    """
    from .config import ROUND_LIMIT_MIN_S, ROUND_LIMIT_MAX_S, ROUND_DECAY, ROUND_INITIAL_S

    limit_s = max(ROUND_LIMIT_MIN_S, ROUND_INITIAL_S - ROUND_DECAY * round_index)
    limit_s = min(ROUND_LIMIT_MAX_S, limit_s)
    return int(limit_s * 1000)
