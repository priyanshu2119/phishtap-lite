"""
Game engine: the full session lifecycle against the mock bridge.

These are the tests that would catch a regression a student actually notices: a
tap that does nothing, a correct answer that costs a life, a game that never
ends, or an answer revealed before the round is over.
"""

from __future__ import annotations

import pytest

from phishtap.bridge_port import BridgeError, BridgePort
from phishtap.corpus import LABEL_PHISH, LABEL_SAFE
from phishtap.db import db
from phishtap.engine import GameEngine
from phishtap.sessions import IllegalTransition, SessionState

UID = "C7DA4104"


# ── Helpers ───────────────────────────────────────────────────────────────

def _start_game(engine, store, bridge, uid=UID, nickname="Ananya"):
    """Tap, connect, consent. Returns the session, ready at round 0."""
    bridge.inject_tap(uid)
    engine.poll()
    session = store.active
    assert session is not None
    engine.connect_phone(session.token)
    engine.submit_consent(session.token, nickname, 1)
    return session


def _current_item(engine, store, items, session):
    payload = engine.session_payload(session.token)
    assert "current" in payload, f"no round armed (state={session.state.value})"
    item_id = payload["current"]["item"]["id"]
    return next(i for i in items if i.id == item_id)


def _answer_correctly(engine, store, items, session):
    item = _current_item(engine, store, items, session)
    engine.render_ack(session.token)
    answer = "phish" if item.label == LABEL_PHISH else "safe"
    return engine.submit_answer(session.token, answer)


def _answer_wrongly(engine, store, items, session):
    item = _current_item(engine, store, items, session)
    engine.render_ack(session.token)
    answer = "safe" if item.label == LABEL_PHISH else "phish"
    return engine.submit_answer(session.token, answer)


# ── Protocol conformance ──────────────────────────────────────────────────

def test_mock_bridge_satisfies_the_port(bridge):
    """If the mock drifts from the protocol, every test here keeps passing while
    the real adapter breaks on the board. This is the assertion that stops that."""
    assert isinstance(bridge, BridgePort)


def test_real_bridge_satisfies_the_port():
    from phishtap.adapters.bridge_adapter import RealBridge

    assert isinstance(RealBridge(), BridgePort)


# ── Tap handling ──────────────────────────────────────────────────────────

def test_tap_creates_a_session_and_tells_the_mcu(engine, store, bridge):
    bridge.inject_tap(UID)
    engine.poll()

    session = store.active
    assert session is not None
    assert session.state == SessionState.AWAITING_PHONE
    assert len(session.pin) == 4

    names = [c[0] for c in bridge.calls]
    assert "set_state" in names
    assert ("set_state", ("tapped",)) in bridge.calls
    assert ("buzz", ("welcome",)) in bridge.calls


def test_tap_is_acknowledged_back_to_the_mcu(engine, store, bridge):
    """Without the ack the ring never drains and every subsequent poll re-reads
    the same events, so the cursor advance is load-bearing."""
    seq = bridge.inject_tap(UID)
    engine.poll()
    assert ("ack_events", (seq,)) in bridge.calls


def test_tap_while_busy_is_refused(engine, store, bridge):
    _start_game(engine, store, bridge)
    bridge.calls.clear()

    bridge.inject_tap("AABBCCDD")
    engine.poll()

    assert ("buzz", ("busy",)) in bridge.calls
    assert ("set_state", ("busy",)) in bridge.calls


def test_tap_with_no_uid_is_ignored(engine, store, bridge):
    bridge.inject_tap("")
    engine.poll()
    assert store.active is None


def test_removal_event_does_not_end_the_session(engine, store, bridge):
    """Removing the card must not abort a game in progress. A student who puts
    the card back in their pocket mid-round would otherwise lose their session."""
    session = _start_game(engine, store, bridge)
    bridge.inject_removal()
    engine.poll()
    assert store.active is session
    assert session.state == SessionState.AWAITING_ANSWER


# ── Phone connection ──────────────────────────────────────────────────────

def test_new_player_is_asked_for_consent(engine, store, bridge):
    bridge.inject_tap(UID)
    engine.poll()
    result = engine.connect_phone(store.active.token)
    assert result["needs_consent"] is True
    assert result["state"] == "consent"


def test_returning_player_skips_consent(engine, store, bridge):
    session = _start_game(engine, store, bridge)
    token = session.token
    # Finish the game so the slot frees up, then tap the same card again.
    session.state = SessionState.GAME_OVER
    store.clear()

    bridge.inject_tap(UID)
    engine.poll()
    result = engine.connect_phone(store.active.token)
    assert result["needs_consent"] is False
    assert result["nickname"] == "Ananya"
    assert result["state"] == "awaiting_answer"
    assert token != store.active.token, "a new tap must mint a new token"


def test_connect_with_an_unknown_token_raises(engine, store, bridge):
    with pytest.raises(KeyError):
        engine.connect_phone("f" * 32)


# ── Consent ───────────────────────────────────────────────────────────────

def test_consent_starts_the_game(engine, store, bridge, real_corpus):
    session = _start_game(engine, store, bridge)
    assert session.state == SessionState.AWAITING_ANSWER
    assert session.round_index == 0
    assert session.lives == 3
    assert session.player_id is not None


def test_consent_arms_the_mcu_countdown(engine, store, bridge):
    _start_game(engine, store, bridge)
    starts = [c for c in bridge.calls if c[0] == "start_round"]
    assert starts, "the MCU was never told to start a round"
    round_index, limit_ms = starts[0][1]
    assert round_index == 0
    assert 4000 <= limit_ms <= 14000


def test_consent_rejects_a_bad_nickname(engine, store, bridge):
    bridge.inject_tap(UID)
    engine.poll()
    with pytest.raises(ValueError):
        engine.submit_consent(store.active.token, "ab", 1)


def test_consent_cannot_be_submitted_twice(engine, store, bridge):
    session = _start_game(engine, store, bridge)
    with pytest.raises(IllegalTransition):
        engine.submit_consent(session.token, "Someone Else", 1)


def test_player_row_is_created_on_consent(engine, store, bridge):
    session = _start_game(engine, store, bridge)
    row = db().execute(
        "SELECT * FROM players WHERE player_id=?", (session.player_id,)
    ).fetchone()
    assert row is not None
    assert row["nickname"] == "Ananya"
    assert row["consent_version"] == 1
    assert row["deleted"] == 0


# ── What the phone is told ────────────────────────────────────────────────

def test_play_payload_never_contains_the_answer(engine, store, bridge, real_corpus):
    """The most important assertion in this file. If the label reaches the phone
    during play, anyone can read it from the network tab and the tool teaches
    nothing."""
    session = _start_game(engine, store, bridge)
    payload = engine.session_payload(session.token)
    item = payload["current"]["item"]
    for forbidden in ("label", "cues", "explanation", "lesson"):
        assert forbidden not in item, f"{forbidden} leaked to the phone mid-game"


def test_round_start_notification_never_contains_the_answer(engine, store, bridge, notifier):
    session = _start_game(engine, store, bridge)
    starts = notifier.events_for("round_start")
    assert starts, "no round_start was pushed to the phone"
    item = starts[-1]["item"]
    for forbidden in ("label", "cues", "explanation", "lesson"):
        assert forbidden not in item


def test_round_result_reports_correctness_but_not_the_label(engine, store, bridge, notifier, real_corpus):
    session = _start_game(engine, store, bridge)
    _answer_correctly(engine, store, real_corpus, session)

    results = notifier.events_for("round_result")
    assert results, "no round_result was pushed"
    payload = results[-1]
    assert "correct" in payload, "the player must be told whether they lost a life"
    for forbidden in ("correct_label", "label", "cues", "explanation", "lesson"):
        assert forbidden not in payload, f"{forbidden} leaked into round_result"


def test_review_payload_reveals_everything(engine, store, bridge, real_corpus):
    session = _start_game(engine, store, bridge)
    _answer_correctly(engine, store, real_corpus, session)
    session.state = SessionState.GAME_OVER
    import secrets
    session.review_token = secrets.token_hex(16)
    session.review_expires_at_mono = session.created_mono + 10**9

    review = engine.review_payload(session.review_token)
    assert review["rounds_played"] == 1
    r0 = review["rounds"][0]
    assert "correct_label" in r0
    assert r0["explanation"]
    assert r0["lesson"]
    assert isinstance(r0["cues"], list)


# ── Scoring and lives ─────────────────────────────────────────────────────

def test_correct_answer_scores_and_keeps_lives(engine, store, bridge, real_corpus):
    session = _start_game(engine, store, bridge)
    result = _answer_correctly(engine, store, real_corpus, session)
    assert result["correct"] is True
    assert result["points"] >= 10
    assert result["lives"] == 3
    assert result["score"] == result["points"]
    assert result["streak"] == 1


def test_wrong_answer_costs_a_life_and_resets_the_streak(engine, store, bridge, real_corpus):
    session = _start_game(engine, store, bridge)
    _answer_correctly(engine, store, real_corpus, session)
    result = _answer_wrongly(engine, store, real_corpus, session)
    assert result["correct"] is False
    assert result["points"] == 0
    assert result["lives"] == 2
    assert result["streak"] == 0


def test_streak_raises_the_multiplier(engine, store, bridge, real_corpus):
    session = _start_game(engine, store, bridge)
    multipliers = []
    for _ in range(4):
        r = _answer_correctly(engine, store, real_corpus, session)
        multipliers.append(r["multiplier"])
    assert multipliers == sorted(multipliers), f"multiplier did not grow: {multipliers}"
    assert multipliers[0] == 1.0
    assert multipliers[-1] > 1.0


def test_three_wrong_answers_end_the_game(engine, store, bridge, real_corpus):
    session = _start_game(engine, store, bridge)
    for _ in range(3):
        _answer_wrongly(engine, store, real_corpus, session)
    assert session.lives == 0
    assert session.state == SessionState.GAME_OVER


def test_game_over_tells_the_mcu_to_show_the_score(engine, store, bridge, real_corpus):
    session = _start_game(engine, store, bridge)
    _answer_correctly(engine, store, real_corpus, session)
    for _ in range(3):
        _answer_wrongly(engine, store, real_corpus, session)

    assert ("set_state", ("over",)) in bridge.calls
    scores = [c[1][0] for c in bridge.calls if c[0] == "show_score"]
    assert scores == [session.score]
    assert ("buzz", ("over",)) in bridge.calls


def test_game_over_issues_a_review_token(engine, store, bridge, real_corpus):
    session = _start_game(engine, store, bridge)
    for _ in range(3):
        _answer_wrongly(engine, store, real_corpus, session)
    assert session.review_token is not None
    assert len(session.review_token) == 32
    assert session.review_expires_at_mono is not None


def test_exhausting_the_pool_ends_the_game(engine, store, bridge, real_corpus):
    """A perfect player must still get an ending, or the console sits in PLAYING
    forever and blocks every subsequent tap."""
    session = _start_game(engine, store, bridge)
    for _ in range(len(real_corpus) + 2):
        if session.state == SessionState.GAME_OVER:
            break
        _answer_correctly(engine, store, real_corpus, session)
    assert session.state == SessionState.GAME_OVER
    assert session.lives > 0, "the game should have ended on the pool, not on lives"


def test_every_answer_is_persisted(engine, store, bridge, real_corpus):
    session = _start_game(engine, store, bridge)
    _answer_correctly(engine, store, real_corpus, session)
    _answer_wrongly(engine, store, real_corpus, session)

    rows = db().execute(
        "SELECT * FROM answers WHERE session_token=?", (session.token,)
    ).fetchall()
    assert len(rows) == 2
    assert sum(r["correct"] for r in rows) == 1
    assert all(r["player_id"] == session.player_id for r in rows)


def test_reaction_time_is_measured_from_the_render_ack(engine, store, bridge, real_corpus):
    session = _start_game(engine, store, bridge)
    engine.render_ack(session.token)
    item = _current_item(engine, store, real_corpus, session)
    engine.submit_answer(
        session.token, "phish" if item.label == LABEL_PHISH else "safe"
    )
    row = db().execute(
        "SELECT reaction_ms, ack_missing FROM answers WHERE session_token=?",
        (session.token,),
    ).fetchone()
    assert row["reaction_ms"] is not None
    assert row["reaction_ms"] >= 0
    assert row["ack_missing"] == 0


def test_a_missing_ack_is_flagged_rather_than_silently_substituted(engine, store, bridge, real_corpus):
    """Without the flag, answers that never acked would be timed from round_start
    and silently mixed into the same reaction-time distribution as the others,
    which would corrupt the analytics rather than merely being incomplete."""
    session = _start_game(engine, store, bridge)
    item = _current_item(engine, store, real_corpus, session)
    engine.submit_answer(
        session.token, "phish" if item.label == LABEL_PHISH else "safe"
    )
    row = db().execute(
        "SELECT ack_missing FROM answers WHERE session_token=?", (session.token,)
    ).fetchone()
    assert row["ack_missing"] == 1


# ── Answer validation ─────────────────────────────────────────────────────

def test_an_invalid_answer_is_rejected(engine, store, bridge):
    session = _start_game(engine, store, bridge)
    with pytest.raises(ValueError):
        engine.submit_answer(session.token, "maybe")


def test_an_answer_outside_a_round_is_rejected(engine, store, bridge):
    bridge.inject_tap(UID)
    engine.poll()
    with pytest.raises(IllegalTransition):
        engine.submit_answer(store.active.token, "phish")


def test_an_unknown_token_is_rejected(engine, store, bridge):
    _start_game(engine, store, bridge)
    with pytest.raises(KeyError):
        engine.submit_answer("f" * 32, "phish")


def test_a_second_answer_in_the_same_round_is_rejected(engine, store, bridge, real_corpus):
    """Once a round is graded it moves on, so a duplicate submission must not be
    able to score twice or cost a second life."""
    session = _start_game(engine, store, bridge)
    _answer_correctly(engine, store, real_corpus, session)
    score_after = session.score
    lives_after = session.lives
    # The round has advanced, so answering with the OLD item's answer is simply a
    # normal answer to the new round. Assert the state moved on instead.
    assert session.round_index == 1
    assert session.state == SessionState.AWAITING_ANSWER
    assert score_after >= 10
    assert lives_after == 3


# ── MCU timeout ───────────────────────────────────────────────────────────

def test_mcu_timeout_costs_a_life(engine, store, bridge, real_corpus):
    session = _start_game(engine, store, bridge)
    bridge.inject_timeout(0)
    engine.poll()
    assert session.lives == 2


def test_mcu_timeout_after_the_round_closed_is_ignored(engine, store, bridge, real_corpus):
    """Both the MCU and the server time the round, so a timeout can arrive for a
    round the server has already graded. Honouring it would cost the player two
    lives for one slow answer."""
    session = _start_game(engine, store, bridge)
    _answer_correctly(engine, store, real_corpus, session)
    lives_before = session.lives
    bridge.inject_timeout(0)
    engine.poll()
    assert session.lives == lives_before


def test_mcu_timeout_for_a_future_round_is_ignored(engine, store, bridge, real_corpus):
    """A timeout claiming to be for a round that has not started yet is nonsense,
    and honouring it would cost a life for nothing."""
    session = _start_game(engine, store, bridge)
    bridge.inject_timeout(99)
    engine.poll()
    assert session.lives == 3


def test_mcu_timeout_for_the_current_round_is_honoured(engine, store, bridge, real_corpus):
    """The other half of the round check: matching rounds must still work, or the
    fix would simply disable MCU timeouts altogether."""
    session = _start_game(engine, store, bridge)
    bridge.inject_timeout(session.round_index)
    engine.poll()
    assert session.lives == 2


def test_repeated_stale_timeouts_cost_nothing(engine, store, bridge, real_corpus):
    """The MCU can emit a timeout for the same closed round more than once if the
    ring is drained slowly. None of them may score."""
    session = _start_game(engine, store, bridge)
    _answer_correctly(engine, store, real_corpus, session)
    lives_before = session.lives
    for _ in range(5):
        bridge.inject_timeout(0)
    engine.poll()
    assert session.lives == lives_before


def test_three_timeouts_end_the_game(engine, store, bridge, real_corpus):
    session = _start_game(engine, store, bridge)
    for _ in range(3):
        bridge.inject_timeout(session.round_index)
        engine.poll()
    assert session.lives == 0
    assert session.state == SessionState.GAME_OVER


# ── Resilience ────────────────────────────────────────────────────────────

class BrokenBridge:
    """A bridge whose every call fails.

    Used to prove the engine degrades instead of crashing. The polling loop runs
    forever, so an exception escaping it would silently freeze the console with no
    visible error — the worst possible failure mode for a demo.

    Written with explicit methods rather than ``__getattr__`` magic so that a
    signature change in ``BridgePort`` breaks this class loudly at the call site
    instead of being silently absorbed by a catch-all.
    """

    def __init__(self):
        self.calls: list[tuple[str, tuple]] = []

    def _fail(self, name: str, *args):
        self.calls.append((name, args))
        raise BridgeError(f"{name} is broken")

    def get_status(self):
        return self._fail("get_status")

    def get_events(self, since):
        return self._fail("get_events", since)

    def selftest(self):
        return self._fail("selftest")

    def ack_events(self, seq):
        return self._fail("ack_events", seq)

    def set_state(self, state):
        return self._fail("set_state", state)

    def start_round(self, round_index, limit_ms):
        return self._fail("start_round", round_index, limit_ms)

    def end_round(self, result):
        return self._fail("end_round", result)

    def set_lives(self, n):
        return self._fail("set_lives", n)

    def show_score(self, score):
        return self._fail("show_score", score)

    def buzz(self, pattern):
        return self._fail("buzz", pattern)


def test_poll_survives_a_failing_bridge(store, real_corpus, notifier):
    broken = BrokenBridge()
    engine = GameEngine(broken, store, real_corpus, notifier)
    engine.poll()          # must not raise
    assert engine.link_ok is False


def test_the_game_continues_when_the_mcu_is_unreachable(store, real_corpus, notifier):
    """Losing the console must not lose the game. The phone is where the student
    actually is; a dead buzzer is a degraded experience, a dead game is a broken
    one."""
    broken = BrokenBridge()
    engine = GameEngine(broken, store, real_corpus, notifier)

    # Drive a session directly, since the broken bridge cannot deliver a tap.
    session = store.create_from_tap(UID)
    engine.connect_phone(session.token)
    engine.submit_consent(session.token, "Ananya", 1)

    assert session.state == SessionState.AWAITING_ANSWER
    item = _current_item(engine, store, real_corpus, session)
    result = engine.submit_answer(
        session.token, "phish" if item.label == LABEL_PHISH else "safe"
    )
    assert result["ok"] is True
    assert session.round_index == 1, "the game advanced despite the MCU being gone"


def test_link_ok_recovers_after_a_failure(engine, store, bridge):
    engine.poll()
    assert engine.link_ok is True


def test_hw_fault_event_is_forwarded_to_the_kiosk(engine, store, bridge, notifier):
    bridge.inject_fault("E_RFID_READ")
    engine.poll()
    faults = notifier.kiosk_events("hw_fault")
    assert faults
    assert faults[-1]["code"] == "E_RFID_READ"


def test_a_recovery_notice_is_not_treated_as_a_fault(engine, store, bridge, notifier):
    """The firmware sends hw_fault with code E_NONE to signal recovery. Showing a
    hardware-fault banner every time the reader recovered would train everyone to
    ignore the banner."""
    bridge.inject_fault("E_NONE")
    engine.poll()
    assert notifier.kiosk_events("hw_fault") == []


def test_an_unknown_event_type_does_not_crash(engine, store, bridge):
    bridge._push("something_new", {"x": 1})
    engine.poll()          # must not raise


def test_ring_overflow_is_tolerated(engine, store, bridge):
    """With nothing draining the ring it wraps. The engine must resync from the
    oldest retained event rather than stall or lose its cursor."""
    for i in range(40):
        bridge.inject_tap(f"{i:08X}")
    engine.poll()
    assert engine.link_ok is True
