"""
PhishTap Lite — analytics queries.

Read-only reporting over the answers, sessions and tap_log tables. Nothing here
mutates state, so every function is safe to call from a request handler on any
thread.

Three questions the architecture asks this module to answer (§8):

  1. Which phishing CUES do students actually miss? Accuracy broken down by cue
     type, per player and across the cohort. This is the pedagogical output — it
     says whether the session taught anything.
  2. How fast do they answer? Reaction-time distribution, split by
     ``answer_source`` so phone and button timings are never mixed. They measure
     different things (a button press includes no render latency) and averaging
     them would produce a number that describes neither.
  3. Where do people drop off? Taps -> sessions started -> sessions completed.
     A tap that never becomes a session is a QR code nobody could scan, which is
     the most common real-world failure at a demo and the one worth measuring.

All durations come from monotonic deltas stored at write time. The board has no
RTC, so wall-clock columns are for human reading only and are never subtracted.
"""

from __future__ import annotations

import json
from typing import Any

from .corpus import LABEL_PHISH
from .db import db


def cohort_accuracy() -> dict[str, Any]:
    """Overall accuracy, and the split between phish and safe items.

    The split matters more than the total. A student who answers "phish" to
    everything scores 50% on a balanced corpus while learning nothing, so a
    single accuracy number can hide exactly the behaviour the tool exists to
    correct.
    """
    row = db().execute(
        """SELECT COUNT(*) AS total,
                  COALESCE(SUM(correct), 0) AS right_answers,
                  COALESCE(SUM(timed_out), 0) AS timeouts
           FROM answers"""
    ).fetchone()

    total = row["total"] or 0
    by_label = {}
    for label, name in ((LABEL_PHISH, "phish"), (0, "safe")):
        r = db().execute(
            """SELECT COUNT(*) AS n, COALESCE(SUM(correct), 0) AS c
               FROM answers WHERE label_correct = ?""",
            (label,),
        ).fetchone()
        n = r["n"] or 0
        by_label[name] = {
            "seen": n,
            "correct": r["c"] or 0,
            "accuracy": round((r["c"] or 0) / n, 4) if n else None,
        }

    return {
        "total_answers": total,
        "correct": row["right_answers"] or 0,
        "accuracy": round((row["right_answers"] or 0) / total, 4) if total else None,
        "timeouts": row["timeouts"] or 0,
        "timeout_rate": round((row["timeouts"] or 0) / total, 4) if total else None,
        "by_label": by_label,
    }


def accuracy_by_cue() -> list[dict[str, Any]]:
    """Accuracy broken down by the cue the item was testing.

    The cue list is stored as JSON text on the items table, so this joins answers
    to items and expands the array in Python rather than in SQL. SQLite has no
    portable JSON table function across the versions likely to be present, and the
    dataset is small enough that expanding in Python is both clearer and fast.

    An item with several cues contributes to each of them, so the counts across
    rows sum to more than the number of answers. That is intended: the question is
    "when urgency was present, did they notice?", not "which single cue was it".
    """
    rows = db().execute(
        """SELECT a.correct, a.timed_out, i.cues
           FROM answers a JOIN items i ON i.id = a.item_id"""
    ).fetchall()

    tally: dict[str, dict[str, int]] = {}
    for r in rows:
        try:
            cues = json.loads(r["cues"]) if r["cues"] else []
        except (json.JSONDecodeError, TypeError):
            # A malformed cues field must not break the whole report. Skip the
            # row and keep going; the alternative is an analytics endpoint that
            # 500s because one content item was edited badly.
            continue
        if not isinstance(cues, list):
            continue
        for cue in cues:
            if not isinstance(cue, str):
                continue
            bucket = tally.setdefault(cue, {"seen": 0, "correct": 0, "timeouts": 0})
            bucket["seen"] += 1
            bucket["correct"] += int(bool(r["correct"]))
            bucket["timeouts"] += int(bool(r["timed_out"]))

    out = []
    for cue, t in sorted(tally.items(), key=lambda kv: -kv[1]["seen"]):
        out.append({
            "cue": cue,
            "seen": t["seen"],
            "correct": t["correct"],
            "accuracy": round(t["correct"] / t["seen"], 4) if t["seen"] else None,
            "timeouts": t["timeouts"],
        })
    return out


def player_accuracy(player_id: str) -> dict[str, Any]:
    """One player's accuracy, overall and by cue."""
    overall = db().execute(
        """SELECT COUNT(*) AS n, COALESCE(SUM(correct), 0) AS c
           FROM answers WHERE player_id = ?""",
        (player_id,),
    ).fetchone()
    n = overall["n"] or 0

    rows = db().execute(
        """SELECT a.correct, i.cues FROM answers a
           JOIN items i ON i.id = a.item_id
           WHERE a.player_id = ?""",
        (player_id,),
    ).fetchall()

    per_cue: dict[str, list[int]] = {}
    for r in rows:
        try:
            cues = json.loads(r["cues"]) if r["cues"] else []
        except (json.JSONDecodeError, TypeError):
            continue
        for cue in cues if isinstance(cues, list) else []:
            if not isinstance(cue, str):
                continue
            seen, correct = per_cue.get(cue, (0, 0))
            per_cue[cue] = (seen + 1, correct + int(bool(r["correct"])))

    return {
        "player_id": player_id,
        "answers": n,
        "correct": overall["c"] or 0,
        "accuracy": round((overall["c"] or 0) / n, 4) if n else None,
        "by_cue": [
            {
                "cue": cue,
                "seen": seen,
                "correct": correct,
                "accuracy": round(correct / seen, 4) if seen else None,
            }
            for cue, (seen, correct) in sorted(per_cue.items(), key=lambda kv: -kv[1][0])
        ],
    }


def reaction_time_distribution() -> dict[str, Any]:
    """Reaction times, split by answer_source and never mixed.

    Reported as percentiles rather than a mean. Reaction time is heavily
    right-skewed — a few students sit and read for the full limit — so the mean
    sits between "typical" and "slowest" and describes neither. p50 and p90 are
    the numbers that actually tell you whether the round timer is fair.

    Rows with ``ack_missing`` are EXCLUDED. Those were timed from round_start
    rather than from the phone's render ack, so they include network and render
    latency that the others do not. Mixing them would widen the distribution for
    a reason that has nothing to do with how fast anyone read anything.
    """
    rows = db().execute(
        """SELECT answer_source, reaction_ms FROM answers
           WHERE reaction_ms IS NOT NULL AND ack_missing = 0
           ORDER BY reaction_ms"""
    ).fetchall()

    by_source: dict[str, list[int]] = {}
    for r in rows:
        by_source.setdefault(r["answer_source"], []).append(int(r["reaction_ms"]))

    def summarise(values: list[int]) -> dict[str, Any]:
        if not values:
            return {"n": 0}
        ordered = sorted(values)
        return {
            "n": len(ordered),
            "min_ms": ordered[0],
            "p50_ms": _percentile(ordered, 50),
            "p90_ms": _percentile(ordered, 90),
            "max_ms": ordered[-1],
        }

    excluded = db().execute(
        "SELECT COUNT(*) n FROM answers WHERE ack_missing = 1"
    ).fetchone()["n"]

    return {
        "by_source": {src: summarise(v) for src, v in sorted(by_source.items())},
        "excluded_ack_missing": excluded,
    }


def _percentile(ordered: list[int], pct: int) -> int:
    """Nearest-rank percentile over an already-sorted list.

    Nearest-rank rather than interpolation because the values are integer
    milliseconds and an interpolated .5 ms is false precision. For a list of one
    element every percentile is that element, which is the sensible answer.
    """
    if not ordered:
        return 0
    if len(ordered) == 1:
        return ordered[0]
    # Nearest-rank: the value at ceil(pct/100 * n), 1-based, clamped.
    rank = max(1, min(len(ordered), -(-pct * len(ordered) // 100)))
    return ordered[rank - 1]


def funnel() -> dict[str, Any]:
    """Taps -> sessions created -> players identified -> games completed.

    FOUR stages, not three, and the extra one is the reason this function is
    useful. A single "started" count defined as ``player_id IS NOT NULL``
    silently merges two completely different failures:

      * a tap where no phone ever connected  -> the QR code could not be scanned,
        or the phone could not reach the board (a NETWORK problem)
      * a tap where the phone connected but the student bailed at the consent
        screen -> the form was confusing or too long (a DESIGN problem)

    Those need different fixes, and a merged number tells you neither. Splitting
    them costs one query.

    The gap between taps and completed is where a demo actually fails, and it is
    the thing worth measuring: counting only completed games would hide all of it.
    """
    taps = db().execute("SELECT COUNT(*) n FROM tap_log").fetchone()["n"]
    created = db().execute("SELECT COUNT(*) n FROM sessions").fetchone()["n"]
    identified = db().execute(
        "SELECT COUNT(*) n FROM sessions WHERE player_id IS NOT NULL"
    ).fetchone()["n"]
    completed = db().execute(
        "SELECT COUNT(*) n FROM sessions WHERE game_over_mono IS NOT NULL"
    ).fetchone()["n"]
    expired = db().execute(
        "SELECT COUNT(*) n FROM sessions WHERE state = 'expired'"
    ).fetchone()["n"]

    def rate(num: int, den: int) -> float | None:
        return round(num / den, 4) if den else None

    return {
        "taps": taps,
        "sessions_created": created,
        "sessions_started": identified,
        "sessions_completed": completed,
        "sessions_expired": expired,
        "tap_to_create": rate(created, taps),
        # The QR-scan / network step.
        "create_to_start": rate(identified, created),
        # The consent-screen step.
        "start_to_complete": rate(completed, identified),
        "tap_to_complete": rate(completed, taps),
    }


def full_report() -> dict[str, Any]:
    """Everything above, in one payload, for the admin panel."""
    return {
        "cohort": cohort_accuracy(),
        "by_cue": accuracy_by_cue(),
        "reaction_ms": reaction_time_distribution(),
        "funnel": funnel(),
    }
