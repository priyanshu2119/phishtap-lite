"""
Session lifecycle: tokens, PINs, TTL, state transitions and persistence.

Time is controlled by monkeypatching ``phishtap.sessions.monotonic_ms`` rather
than by sleeping, so a 90-second TTL can be tested in microseconds and, more
importantly, deterministically.
"""

from __future__ import annotations

import pytest

from phishtap import sessions as sessions_mod
from phishtap.config import SESSION_TTL_S
from phishtap.db import db
from phishtap.sessions import (
    IllegalTransition,
    Session,
    SessionState,
    SessionStore,
)

UID = "C7DA4104"


@pytest.fixture
def clock(monkeypatch):
    """A controllable monotonic clock, in milliseconds."""
    state = {"now": 1_000_000}
    monkeypatch.setattr(sessions_mod, "monotonic_ms", lambda: state["now"])
    state["advance"] = lambda ms: state.__setitem__("now", state["now"] + ms)
    return state


# ── Creation ──────────────────────────────────────────────────────────────

def test_tap_creates_a_session(store, clock):
    s = store.create_from_tap(UID)
    assert s is not None
    assert s.uid_hex == UID
    assert s.state == SessionState.AWAITING_PHONE
    assert s.lives == 3
    assert s.score == 0


def test_token_is_128_bits_of_hex(store, clock):
    """The architecture specifies a 128-bit token. 32 hex characters is exactly
    128 bits; anything shorter would be guessable on a classroom network."""
    s = store.create_from_tap(UID)
    assert len(s.token) == 32
    assert all(c in "0123456789abcdef" for c in s.token)


def test_tokens_are_unique(store, clock):
    tokens = set()
    for _ in range(50):
        s = store.create_from_tap(UID)
        tokens.add(s.token)
        store.clear()
    assert len(tokens) == 50, "token generation repeated a value"


def test_pin_is_four_digits(store, clock):
    s = store.create_from_tap(UID)
    assert len(s.pin) == 4
    assert s.pin.isdigit()


def test_pin_is_zero_padded(store, clock):
    """A PIN below 1000 must still render as four digits, or the kiosk would show
    '47' and a student typing it would fail."""
    seen_short = False
    for _ in range(400):
        s = store.create_from_tap(UID)
        if int(s.pin) < 1000:
            assert len(s.pin) == 4, f"PIN {s.pin!r} is not zero-padded"
            seen_short = True
        store.clear()
    assert seen_short, "never generated a PIN below 1000 in 400 tries; test is weak"


def test_only_one_active_session(store, clock):
    """A second tap while a session is live must not replace it, or a student
    mid-game would lose their progress because someone else tapped."""
    first = store.create_from_tap(UID)
    second = store.create_from_tap("AABBCCDD")
    assert second is None
    assert store.active is first


def test_expiry_frees_the_slot(store, clock):
    store.create_from_tap(UID)
    clock["advance"](int(SESSION_TTL_S * 1000) + 1)
    assert store.create_from_tap("AABBCCDD") is not None


# ── TTL ───────────────────────────────────────────────────────────────────

def test_session_is_not_expired_before_the_ttl(store, clock):
    s = store.create_from_tap(UID)
    clock["advance"](int(SESSION_TTL_S * 1000) - 1000)
    assert s.is_expired(clock["now"]) is False
    assert store.expire_if_needed() is None


def test_session_expires_at_the_ttl(store, clock):
    s = store.create_from_tap(UID)
    clock["advance"](int(SESSION_TTL_S * 1000))
    assert s.is_expired(clock["now"]) is True


def test_expire_if_needed_clears_the_slot(store, clock):
    store.create_from_tap(UID)
    clock["advance"](int(SESSION_TTL_S * 1000) + 1)
    expired = store.expire_if_needed()
    assert expired is not None
    assert expired.state == SessionState.EXPIRED
    assert store.active is None


def test_an_active_game_does_not_expire(store, clock):
    """Once a phone has connected the session must not time out mid-game. The TTL
    exists to reap sessions nobody joined, not to end games that are being
    played."""
    s = store.create_from_tap(UID)
    s.transition(SessionState.ROUND_START)
    s.transition(SessionState.AWAITING_ANSWER)
    clock["advance"](int(SESSION_TTL_S * 1000) * 10)
    assert s.is_expired(clock["now"]) is False
    assert store.expire_if_needed() is None


def test_is_busy_reflects_the_session_state(store, clock):
    assert store.is_busy() is False
    s = store.create_from_tap(UID)
    assert store.is_busy() is True
    s.state = SessionState.GAME_OVER
    assert store.is_busy() is False
    s.state = SessionState.REVIEW
    assert store.is_busy() is False


def test_is_busy_is_false_after_expiry(store, clock):
    """A tap arriving just after a TTL lapse must not be refused as busy, or the
    console would appear stuck for no visible reason."""
    store.create_from_tap(UID)
    clock["advance"](int(SESSION_TTL_S * 1000) + 1)
    assert store.is_busy() is False


# ── Lookups ───────────────────────────────────────────────────────────────

def test_lookup_by_token(store, clock):
    s = store.create_from_tap(UID)
    assert store.get_by_token(s.token) is s
    assert store.get_by_token("deadbeef" * 4) is None
    assert store.get_by_token("") is None


def test_lookup_by_pin(store, clock):
    s = store.create_from_tap(UID)
    assert store.get_by_pin(s.pin) is s
    assert store.get_by_pin("0000" if s.pin != "0000" else "0001") is None
    assert store.get_by_pin("") is None


# ── State machine ─────────────────────────────────────────────────────────

def test_the_happy_path_is_legal():
    s = Session(
        token="t", pin="0000", uid_hex=UID, player_id=None, nickname=None,
        state=SessionState.AWAITING_PHONE,
    )
    for nxt in (
        SessionState.CONSENT,
        SessionState.ROUND_START,
        SessionState.AWAITING_ANSWER,
        SessionState.ROUND_RESULT,
        SessionState.ROUND_START,
        SessionState.AWAITING_ANSWER,
        SessionState.GAME_OVER,
        SessionState.REVIEW,
        SessionState.EXPIRED,
    ):
        s.transition(nxt)
    assert s.state == SessionState.EXPIRED


@pytest.mark.parametrize(
    "start,illegal",
    [
        (SessionState.AWAITING_PHONE, SessionState.GAME_OVER),
        (SessionState.AWAITING_PHONE, SessionState.AWAITING_ANSWER),
        (SessionState.CONSENT, SessionState.GAME_OVER),
        (SessionState.REVIEW, SessionState.ROUND_START),
        (SessionState.EXPIRED, SessionState.AWAITING_PHONE),
    ],
)
def test_illegal_transitions_are_rejected(start, illegal):
    """A session that can jump from AWAITING_PHONE to GAME_OVER is a bug, and
    letting it through silently would hide it until someone noticed missing
    scores."""
    s = Session(
        token="t", pin="0000", uid_hex=UID, player_id=None, nickname=None,
        state=start,
    )
    with pytest.raises(IllegalTransition):
        s.transition(illegal)


def test_transition_to_the_same_state_is_a_no_op():
    s = Session(
        token="t", pin="0000", uid_hex=UID, player_id=None, nickname=None,
        state=SessionState.AWAITING_ANSWER,
    )
    s.transition(SessionState.AWAITING_ANSWER)   # must not raise
    assert s.state == SessionState.AWAITING_ANSWER


def test_expired_is_terminal():
    s = Session(
        token="t", pin="0000", uid_hex=UID, player_id=None, nickname=None,
        state=SessionState.EXPIRED,
    )
    for target in SessionState:
        if target == SessionState.EXPIRED:
            continue
        with pytest.raises(IllegalTransition):
            s.transition(target)


def test_illegal_transition_is_not_a_value_error():
    """Regression guard.

    IllegalTransition used to subclass ValueError, and because Python matches the
    first compatible except clause, every API handler that caught ValueError for
    input validation silently swallowed state violations too. Submitting consent
    twice returned 400 E_NICKNAME instead of 409 E_CONSENT, telling the client
    its nickname was bad when the real problem was that the round had closed.

    Deriving from Exception keeps the two failure classes apart so no ordering of
    except clauses can confuse them again.
    """
    assert not issubclass(IllegalTransition, ValueError)
    assert issubclass(IllegalTransition, Exception)


# ── Persistence ───────────────────────────────────────────────────────────

def test_session_is_written_to_the_database(store, clock):
    s = store.create_from_tap(UID)
    row = db().execute("SELECT * FROM sessions WHERE token=?", (s.token,)).fetchone()
    assert row is not None
    assert row["pin"] == s.pin
    assert row["state"] == "awaiting_phone"


def test_the_tap_is_logged(store, clock):
    s = store.create_from_tap(UID)
    rows = db().execute("SELECT * FROM tap_log").fetchall()
    assert len(rows) == 1
    assert rows[0]["session_token"] == s.token


def test_the_raw_uid_is_never_persisted(store, clock):
    """docs/ARCHITECTURE.md §8: the raw UID exists only in the bridge payload, the
    HMAC input, and then nowhere. This is the assertion that keeps that true —
    it scans every column of every row written by a tap."""
    store.create_from_tap(UID)
    for table in ("sessions", "tap_log", "players", "answers"):
        rows = db().execute(f"SELECT * FROM {table}").fetchall()  # noqa: S608
        for row in rows:
            for key in row.keys():
                value = row[key]
                if isinstance(value, str):
                    assert UID.lower() not in value.lower(), (
                        f"{table}.{key} contains the raw UID"
                    )


def test_card_ref_is_stored_instead(store, clock):
    from phishtap.privacy import card_ref

    s = store.create_from_tap(UID)
    row = db().execute(
        "SELECT card_ref FROM sessions WHERE token=?", (s.token,)
    ).fetchone()
    assert row["card_ref"] == card_ref(UID)


def test_player_id_is_null_until_consent(store, clock):
    """A session is created at tap time, before any nickname exists, so in
    shared-card mode the player_id genuinely cannot be known yet."""
    s = store.create_from_tap(UID)
    row = db().execute(
        "SELECT player_id FROM sessions WHERE token=?", (s.token,)
    ).fetchone()
    assert row["player_id"] is None


def test_state_changes_are_persisted(store, clock):
    s = store.create_from_tap(UID)
    s.transition(SessionState.CONSENT)
    store.persist(s)
    row = db().execute(
        "SELECT state FROM sessions WHERE token=?", (s.token,)
    ).fetchone()
    assert row["state"] == "consent"


def test_clear_frees_the_slot_without_touching_the_row(store, clock):
    s = store.create_from_tap(UID)
    store.clear()
    assert store.active is None
    row = db().execute("SELECT * FROM sessions WHERE token=?", (s.token,)).fetchone()
    assert row is not None, "clearing the in-memory slot must not delete the record"
