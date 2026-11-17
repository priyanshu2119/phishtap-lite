"""
Analytics: the pedagogical output of the whole project.

These tests matter more than they look. Analytics is the one part of PhishTap
whose correctness cannot be observed by playing the game — a cue breakdown that
silently returns empty, or a reaction-time distribution that mixes phone and
button timings, produces a plausible-looking report that is simply wrong. For a
thesis that is worse than a crash, because nobody notices.

Two classes of test:

  * Integration, driving the real engine so answers are written by the same code
    path production uses. This catches a join that does not match how rows are
    actually stored.
  * Edge cases written directly, for inputs the engine cannot produce but the
    database can hold: malformed cue JSON, an item deleted from the corpus, an
    empty database.
"""

from __future__ import annotations

import json

import pytest

from phishtap import analytics
from phishtap.corpus import LABEL_PHISH, LABEL_SAFE
from phishtap.db import db, sync_items_to_db

UID = "C7DA4104"


# ── Helpers ───────────────────────────────────────────────────────────────

def _start_game(engine, store, bridge, uid=UID, nickname="Ananya"):
    bridge.inject_tap(uid)
    engine.poll()
    session = store.active
    assert session is not None
    engine.connect_phone(session.token)
    engine.submit_consent(session.token, nickname, 1)
    return session


def _current_item(engine, items, session):
    payload = engine.session_payload(session.token)
    assert "current" in payload, f"no round armed (state={session.state.value})"
    return next(i for i in items if i.id == payload["current"]["item"]["id"])


def _answer(engine, items, session, *, correctly=True, source="phone"):
    item = _current_item(engine, items, session)
    engine.render_ack(session.token)
    right = "phish" if item.label == LABEL_PHISH else "safe"
    wrong = "safe" if item.label == LABEL_PHISH else "phish"
    return engine.submit_answer(session.token, right if correctly else wrong,
                                source=source)


def _play(engine, store, bridge, items, *, nickname, n_rounds, correctly=True,
          source="phone", uid=UID):
    """Play n_rounds of a game. Returns the session."""
    session = _start_game(engine, store, bridge, uid=uid, nickname=nickname)
    for _ in range(n_rounds):
        if session.state.value == "game_over":
            break
        _answer(engine, items, session, correctly=correctly, source=source)
    return session


def _seed_parents(conn, *, token="t", player_id="p"):
    """Create the parent rows a directly-inserted answer needs.

    ``answers`` has foreign keys to both ``sessions(token)`` and
    ``players(player_id)``, and the connection sets ``PRAGMA foreign_keys=ON``,
    so an orphan insert fails outright rather than quietly producing a row the
    analytics joins would then skip. These tests are about the reporting queries,
    not about session creation, so the parents are deliberately minimal.
    """
    conn.execute(
        "INSERT OR IGNORE INTO players (player_id, nickname, created_mono, created_wall)"
        " VALUES (?,?,?,?)",
        (player_id, "seed", 0, "2026-01-01T00:00:00Z"),
    )
    conn.execute(
        """INSERT OR IGNORE INTO sessions
           (token, pin, player_id, card_ref, state, created_mono, created_wall,
            expires_at_mono)
           VALUES (?,?,?,?,?,?,?,?)""",
        (token, "0000", player_id, "seedcard", "game_over", 0,
         "2026-01-01T00:00:00Z", 0),
    )


def _insert_answer(conn, item_id, *, token="t", player_id="p", correct=1,
                   timed_out=0, source="phone", ack_missing=0, reaction_ms=1200):
    """Insert one answer row with its parents, for edge cases the engine cannot
    produce but the database can hold."""
    _seed_parents(conn, token=token, player_id=player_id)
    conn.execute(
        """INSERT INTO answers
           (session_token, player_id, round_index, item_id, label_correct,
            label_answered, correct, timed_out, limit_ms, remaining_ms,
            reaction_ms, streak_before, speed, multiplier, points,
            answer_source, ack_missing, answered_mono, answered_wall)
           VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
        (token, player_id, 0, item_id, LABEL_PHISH, LABEL_PHISH, correct,
         timed_out, 10000, 5000, reaction_ms, 0, 4, 1.0, 14, source,
         ack_missing, 0, "2026-01-01T00:00:00Z"),
    )


# ── Empty database ────────────────────────────────────────────────────────
#
# The report must be safe to open before anyone has played. An admin panel that
# 500s on a fresh install is an admin panel nobody trusts.

def test_cohort_accuracy_on_an_empty_database_returns_none_not_zero():
    """No answers means accuracy is UNKNOWN, not 0%.

    Reporting 0 would say "every student got everything wrong" about a database
    nobody has used. None is the honest answer and the frontend renders it as a
    dash.
    """
    rep = analytics.cohort_accuracy()
    assert rep["total_answers"] == 0
    assert rep["accuracy"] is None
    assert rep["timeout_rate"] is None
    assert rep["by_label"]["phish"]["accuracy"] is None
    assert rep["by_label"]["safe"]["accuracy"] is None


def test_by_cue_on_an_empty_database_is_an_empty_list():
    assert analytics.accuracy_by_cue() == []


def test_reaction_times_on_an_empty_database_have_no_sources():
    rep = analytics.reaction_time_distribution()
    assert rep["by_source"] == {}
    assert rep["excluded_ack_missing"] == 0


def test_funnel_on_an_empty_database_has_null_rates():
    """A rate with a zero denominator is None, never 0.0 and never a crash."""
    f = analytics.funnel()
    assert f["taps"] == 0
    assert f["sessions_created"] == 0
    assert f["tap_to_create"] is None
    assert f["create_to_start"] is None
    assert f["start_to_complete"] is None
    assert f["tap_to_complete"] is None


def test_full_report_has_every_section_even_when_empty():
    rep = analytics.full_report()
    assert set(rep) == {"cohort", "by_cue", "reaction_ms", "funnel"}


# ── Cohort accuracy ───────────────────────────────────────────────────────

def test_cohort_accuracy_splits_phish_from_safe(engine, store, bridge, real_corpus):
    """The split is the point.

    A student who answers "phish" to everything scores 50% on a balanced corpus
    while learning nothing, so a single accuracy number can hide exactly the
    behaviour the tool exists to correct.
    """
    items = real_corpus
    sync_items_to_db(items)
    _play(engine, store, bridge, items, nickname="Ananya", n_rounds=4, correctly=True)

    rep = analytics.cohort_accuracy()
    assert rep["total_answers"] == 4
    assert rep["correct"] == 4
    assert rep["accuracy"] == 1.0
    # Both labels must have been seen, or the split is vacuous.
    seen = rep["by_label"]["phish"]["seen"] + rep["by_label"]["safe"]["seen"]
    assert seen == 4


def test_wrong_answers_lower_accuracy(engine, store, bridge, real_corpus):
    items = real_corpus
    sync_items_to_db(items)
    # Only three rounds are reachable when every answer is wrong: three lives,
    # one lost per wrong answer, and the game ends at zero. Asking for four
    # rounds and expecting four answers would be asserting that the game ignores
    # its own loss condition.
    _play(engine, store, bridge, items, nickname="Ravi", n_rounds=4, correctly=False)

    rep = analytics.cohort_accuracy()
    assert rep["total_answers"] == 3
    assert rep["correct"] == 0
    assert rep["accuracy"] == 0.0


def test_timeouts_are_counted_separately_from_wrong_answers(
    engine, store, bridge, real_corpus
):
    """A timeout is not the same as a wrong answer, even though both cost a life.

    Timeout rate is the number that says the round timer is too short; wrong
    rate is the number that says the content is too hard. Collapsing them would
    make both unactionable.

    Driven through the MCU's ``round_timeout`` event rather than by poking a
    deadline field, because that is the only path production has: the server
    learns about a timeout from the console, not from its own clock.
    """
    items = real_corpus
    sync_items_to_db(items)
    _start_game(engine, store, bridge, nickname="Meera")

    bridge.inject_timeout(0)
    engine.poll()

    rep = analytics.cohort_accuracy()
    assert rep["timeouts"] == 1
    assert rep["total_answers"] == 1
    assert rep["timeout_rate"] == 1.0
    # A timeout scores zero, so it must not be counted as correct.
    assert rep["correct"] == 0


# ── Cue breakdown ─────────────────────────────────────────────────────────

def test_a_multi_cue_item_counts_toward_every_cue(engine, store, bridge, real_corpus):
    """An item testing urgency AND a lookalike domain contributes to both.

    The question is "when urgency was present, did they notice?", not "which
    single cue was it". So the per-cue counts deliberately sum to more than the
    number of answers, and a test that asserted equality would be asserting the
    wrong invariant.
    """
    items = real_corpus
    sync_items_to_db(items)
    _play(engine, store, bridge, items, nickname="Ananya", n_rounds=4, correctly=True)

    rows = analytics.accuracy_by_cue()
    assert rows, "no cue rows produced"
    total_cue_hits = sum(r["seen"] for r in rows)
    n_answers = analytics.cohort_accuracy()["total_answers"]
    assert total_cue_hits >= n_answers, (
        "each answer must contribute to at least one cue"
    )
    for r in rows:
        assert r["accuracy"] == 1.0
        assert r["correct"] == r["seen"]


def test_cue_rows_are_sorted_by_frequency(engine, store, bridge, real_corpus):
    """Most-seen cue first, so the report leads with the most reliable number.

    A cue seen twice has an accuracy that means almost nothing; putting it at the
    top of the table would invite exactly the wrong conclusion.
    """
    items = real_corpus
    sync_items_to_db(items)
    _play(engine, store, bridge, items, nickname="Ananya", n_rounds=6, correctly=True)

    rows = analytics.accuracy_by_cue()
    seen = [r["seen"] for r in rows]
    assert seen == sorted(seen, reverse=True)


def test_malformed_cue_json_does_not_break_the_whole_report():
    """One badly edited content item must not 500 the admin panel.

    The cues column is JSON text, so a human editing corpus.json can produce
    something unparseable. Skipping the row and continuing is the right trade:
    a slightly incomplete report beats an endpoint that returns nothing.
    """
    conn = db()
    conn.execute(
        "INSERT OR REPLACE INTO items (id, channel, label, difficulty, cues) "
        "VALUES (?,?,?,?,?)",
        ("broken_item", "email", LABEL_PHISH, "easy", "{not valid json"),
    )
    _insert_answer(conn, "broken_item")
    # Must not raise, and must not include the broken item's cues.
    rows = analytics.accuracy_by_cue()
    assert all(r["cue"] for r in rows)


def test_a_non_list_cues_value_is_skipped():
    """cues stored as a JSON string or number is skipped, not iterated char by char.

    Iterating a string would produce one "cue" per character, which is the kind
    of silent nonsense that looks like data.
    """
    conn = db()
    conn.execute(
        "INSERT OR REPLACE INTO items (id, channel, label, difficulty, cues) "
        "VALUES (?,?,?,?,?)",
        ("string_cues", "sms", LABEL_SAFE, "easy", json.dumps("urgency")),
    )
    _insert_answer(conn, "string_cues")
    cues = {r["cue"] for r in analytics.accuracy_by_cue()}
    assert "u" not in cues, "a string was iterated character by character"


def test_an_answer_whose_item_is_missing_from_the_corpus_is_not_counted():
    """An INNER JOIN drops answers whose item was deleted from corpus.json.

    That is the correct behaviour for a cue report: an item with no metadata has
    no cues to attribute the answer to. Counting it under a blank cue would
    invent a category.
    """
    conn = db()
    _insert_answer(conn, "orphan_item")
    rows = analytics.accuracy_by_cue()
    assert all("orphan" not in str(r) for r in rows)


# ── Per-player ────────────────────────────────────────────────────────────

def test_player_accuracy_is_scoped_to_one_player(engine, store, bridge, real_corpus):
    """Two players on the SAME card must not see each other's numbers.

    In shared_card mode the nickname is what separates them, so this is the test
    that the identity design actually holds up in the reporting layer.

    The second game uses a different card UID. Reusing one would be suppressed by
    the three-second per-card cooldown, and the test would then be asserting
    nothing about player scoping at all.
    """
    items = real_corpus
    sync_items_to_db(items)
    a = _play(engine, store, bridge, items, nickname="Ananya", n_rounds=3,
              correctly=True, uid="AAAAAAAA")
    store.clear()
    b = _play(engine, store, bridge, items, nickname="Ravi", n_rounds=3,
              correctly=False, uid="BBBBBBBB")

    assert a.player_id != b.player_id, "shared card collapsed two players into one"

    ra = analytics.player_accuracy(a.player_id)
    rb = analytics.player_accuracy(b.player_id)
    assert ra["answers"] == 3 and ra["correct"] == 3
    assert rb["answers"] == 3 and rb["correct"] == 0
    assert ra["accuracy"] == 1.0 and rb["accuracy"] == 0.0


def test_two_players_sharing_one_card_stay_distinct():
    """The shared_card identity mode, tested directly.

    Same UID, different nicknames, must produce different player_ids. If this
    ever regresses, every student at a demo collapses into one row and the entire
    analytics output becomes meaningless while still looking plausible.

    Tested through the identity function rather than by playing two games, because
    the three-second per-card cooldown legitimately suppresses an immediate
    re-tap of the same card. Driving it through the engine would make this a test
    of the cooldown that happened to mention identity.
    """
    from phishtap.privacy import player_id

    a = player_id(UID, "Ananya")
    b = player_id(UID, "Ravi")
    assert a != b, "two nicknames on one card collapsed into one player"

    # And the same person must map back to the same id, or consent would be
    # re-shown and history lost on every visit.
    assert player_id(UID, "Ananya") == a
    # Case and surrounding whitespace are not a different person.
    assert player_id(UID, "  ananya ") == a


def test_player_accuracy_for_an_unknown_player_is_empty_not_an_error():
    """An admin looking up a deleted or mistyped player_id gets an empty report,
    not a 500."""
    rep = analytics.player_accuracy("doesnotexist0000")
    assert rep["answers"] == 0
    assert rep["correct"] == 0
    assert rep["accuracy"] is None
    assert rep["by_cue"] == []


# ── Reaction times ────────────────────────────────────────────────────────

def test_phone_and_button_timings_are_never_mixed(engine, store, bridge, real_corpus):
    """A button press includes no render latency, so averaging the two produces a
    number that describes neither. The architecture requires the split (§8)."""
    items = real_corpus
    sync_items_to_db(items)
    _play(engine, store, bridge, items, nickname="Ananya", n_rounds=2,
          correctly=True, source="phone", uid="AAAAAAAA")
    store.clear()
    _play(engine, store, bridge, items, nickname="Ravi", n_rounds=2,
          correctly=True, source="button", uid="BBBBBBBB")

    rep = analytics.reaction_time_distribution()
    assert set(rep["by_source"]) == {"phone", "button"}
    assert rep["by_source"]["phone"]["n"] == 2
    assert rep["by_source"]["button"]["n"] == 2


def test_ack_missing_rows_are_excluded_from_reaction_times(
    engine, store, bridge, real_corpus
):
    """Those were timed from round_start, not from the phone's render ack, so they
    include network and render latency the others do not. Mixing them would widen
    the distribution for a reason that has nothing to do with reading speed."""
    items = real_corpus
    sync_items_to_db(items)
    session = _start_game(engine, store, bridge, nickname="Ananya")

    # Answer WITHOUT calling render_ack, which sets ack_missing.
    item = _current_item(engine, items, session)
    answer = "phish" if item.label == LABEL_PHISH else "safe"
    engine.submit_answer(session.token, answer)

    rep = analytics.reaction_time_distribution()
    assert rep["excluded_ack_missing"] == 1
    for src in rep["by_source"].values():
        assert src["n"] == 0, "an ack_missing row leaked into the distribution"


def test_percentiles_are_reported_rather_than_a_mean(engine, store, bridge, real_corpus):
    """Reaction time is right-skewed: a few students read for the full limit. The
    mean then sits between typical and slowest and describes neither."""
    items = real_corpus
    sync_items_to_db(items)
    _play(engine, store, bridge, items, nickname="Ananya", n_rounds=4, correctly=True)

    rep = analytics.reaction_time_distribution()
    phone = rep["by_source"]["phone"]
    for key in ("min_ms", "p50_ms", "p90_ms", "max_ms"):
        assert key in phone
    assert phone["min_ms"] <= phone["p50_ms"] <= phone["p90_ms"] <= phone["max_ms"]
    assert "mean_ms" not in phone


def test_percentile_nearest_rank_on_a_known_list():
    p = analytics._percentile
    ordered = [10, 20, 30, 40, 50, 60, 70, 80, 90, 100]
    assert p(ordered, 50) == 50
    assert p(ordered, 90) == 90
    assert p(ordered, 100) == 100
    assert p(ordered, 1) == 10


def test_percentile_of_a_single_value_is_that_value():
    """Every percentile of one observation is that observation. Interpolating
    here would invent precision the sample does not have."""
    assert analytics._percentile([42], 50) == 42
    assert analytics._percentile([42], 90) == 42


def test_percentile_of_an_empty_list_is_zero_not_a_crash():
    assert analytics._percentile([], 50) == 0


def test_percentile_never_indexes_out_of_range():
    """Nearest-rank uses ceil(pct/100 * n); at pct=100 that is exactly n, so the
    1-based index must be clamped or the last element reads off the end."""
    ordered = list(range(1, 8))
    assert analytics._percentile(ordered, 100) == 7
    assert analytics._percentile(ordered, 999) == 7


# ── Funnel ────────────────────────────────────────────────────────────────

def test_funnel_separates_a_network_failure_from_a_consent_abandonment(
    engine, store, bridge, real_corpus
):
    """The reason the funnel has four stages instead of three.

    One tap is abandoned before any phone connects; another becomes a played
    game. The abandoned one must show up in the gap between ``sessions_created``
    and ``sessions_started``, which is the QR-scan/network step, and NOT be
    confused with someone bailing at the consent screen.
    """
    items = real_corpus
    sync_items_to_db(items)

    # One tap that is abandoned: no phone ever connects. A distinct UID keeps it
    # out of the cooldown window that the played game would otherwise share.
    bridge.inject_tap("ABANDONED")
    engine.poll()
    abandoned = store.active
    assert abandoned is not None, "the abandoned tap did not create a session"
    assert abandoned.player_id is None, "no consent, so no player_id"
    store.clear()

    # One tap that becomes a played game. Answering wrongly ends it after three
    # rounds, because three wrong answers exhaust three lives. Playing only
    # correct answers would leave the session mid-game with game_over_mono still
    # NULL, and the completed count would be zero for a reason that has nothing
    # to do with the funnel.
    _play(engine, store, bridge, items, nickname="Ananya", n_rounds=3,
          correctly=False, uid="PLAYED001")

    f = analytics.funnel()
    assert f["taps"] == 2
    assert f["sessions_created"] == 2, "both taps created a session row"
    assert f["sessions_started"] == 1, "only one ever identified a player"
    assert f["sessions_completed"] == 1, "the played game reached game over"

    # The drop-off is attributed to the right step.
    assert f["tap_to_create"] == 1.0
    assert f["create_to_start"] == 0.5, "the abandoned tap shows up here"
    assert f["start_to_complete"] == 1.0, "nobody bailed at consent"
    assert f["tap_to_complete"] == 0.5


def test_funnel_rates_are_between_zero_and_one(engine, store, bridge, real_corpus):
    items = real_corpus
    sync_items_to_db(items)
    _play(engine, store, bridge, items, nickname="Ananya", n_rounds=3, correctly=True)

    f = analytics.funnel()
    for key, value in f.items():
        if key.endswith(("_to_create", "_to_start", "_to_complete")) or key in (
            "create_to_start", "start_to_complete"
        ):
            assert value is None or 0.0 <= value <= 1.0, f"{key}={value}"


# ── Corpus sync ───────────────────────────────────────────────────────────

def test_sync_items_is_idempotent(real_corpus):
    """Called on every startup, so running it twice must not duplicate or error."""
    n1 = sync_items_to_db(real_corpus)
    n2 = sync_items_to_db(real_corpus)
    assert n1 == n2 == len(real_corpus)
    count = db().execute("SELECT COUNT(*) n FROM items").fetchone()["n"]
    assert count == len(real_corpus)


def test_sync_items_updates_metadata_without_orphaning_answers(real_corpus):
    """An UPSERT, not delete-and-reinsert.

    Deleting would break the foreign key relationship for every answer already
    recorded against that item, silently destroying the history the analytics
    exist to report on.
    """
    sync_items_to_db(real_corpus)
    item = real_corpus[0]

    conn = db()
    _insert_answer(conn, item.id)

    # Re-sync must leave the answer row intact and still joinable.
    sync_items_to_db(real_corpus)
    still_there = conn.execute(
        "SELECT COUNT(*) n FROM answers WHERE item_id = ?", (item.id,)
    ).fetchone()["n"]
    assert still_there == 1

    joined = conn.execute(
        "SELECT COUNT(*) n FROM answers a JOIN items i ON i.id = a.item_id"
    ).fetchone()["n"]
    assert joined >= 1


def test_cues_round_trip_through_the_database_as_json(real_corpus):
    """Cues are stored as JSON text and parsed back in Python. If the round trip
    lost or mangled them, every cue would silently report as unseen."""
    sync_items_to_db(real_corpus)
    multi = next(i for i in real_corpus if len(i.cues) >= 2)

    row = db().execute(
        "SELECT cues FROM items WHERE id = ?", (multi.id,)
    ).fetchone()
    assert json.loads(row["cues"]) == list(multi.cues)
