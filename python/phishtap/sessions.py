"""
PhishTap Lite — session lifecycle.

One active session at a time. A card tap creates one with a 128-bit token and a
4-digit PIN; the phone joins with either; the session expires after 90 s if no
phone connects, and again at game end.

The state machine here is the SERVER side from docs/ARCHITECTURE.md §5. The MCU
has its own, simpler one, and the server wins whenever they disagree — the MCU
renders, the server decides.

Tokens come from ``secrets``, never ``random``. ``random`` is a Mersenne Twister
seeded from the clock and its output is predictable from a handful of observed
values; a session token is a bearer credential, so predicting one would let
anyone on the same Wi-Fi hijack a student's game and read their review page.
"""

from __future__ import annotations

import secrets
import sqlite3
import threading
from dataclasses import dataclass, field
from enum import Enum

from .clock import monotonic_ms, wall_now_iso
from .config import SESSION_TTL_S, STARTING_LIVES
from .db import db, transaction
from .privacy import card_ref, player_id as compute_player_id


class SessionState(str, Enum):
    """Server-side session states.

    Subclasses ``str`` so the value can be JSON-serialised and compared against
    the wire strings without a conversion step at every boundary.
    """

    AWAITING_PHONE = "awaiting_phone"
    CONSENT = "consent"
    ROUND_START = "round_start"
    AWAITING_ANSWER = "awaiting_answer"
    ROUND_RESULT = "round_result"
    GAME_OVER = "game_over"
    REVIEW = "review"
    EXPIRED = "expired"


# Transitions that are legal. Anything not listed here is rejected rather than
# silently applied, because a session that jumps from AWAITING_PHONE to GAME_OVER
# is a bug and letting it through would hide it.
_ALLOWED_TRANSITIONS: dict[SessionState, frozenset[SessionState]] = {
    SessionState.AWAITING_PHONE: frozenset(
        {SessionState.CONSENT, SessionState.ROUND_START, SessionState.EXPIRED}
    ),
    SessionState.CONSENT: frozenset({SessionState.ROUND_START, SessionState.EXPIRED}),
    SessionState.ROUND_START: frozenset(
        {SessionState.AWAITING_ANSWER, SessionState.GAME_OVER, SessionState.EXPIRED}
    ),
    SessionState.AWAITING_ANSWER: frozenset(
        {SessionState.ROUND_RESULT, SessionState.GAME_OVER, SessionState.EXPIRED}
    ),
    SessionState.ROUND_RESULT: frozenset(
        {SessionState.ROUND_START, SessionState.GAME_OVER, SessionState.EXPIRED}
    ),
    SessionState.GAME_OVER: frozenset({SessionState.REVIEW, SessionState.EXPIRED}),
    SessionState.REVIEW: frozenset({SessionState.EXPIRED}),
    SessionState.EXPIRED: frozenset(),
}


class IllegalTransition(Exception):
    """Raised when a session state change is not permitted.

    Deliberately NOT a subclass of ValueError, even though "an invalid state"
    sounds like a bad value. It was originally, and that caused a real bug: the
    API handlers catch ValueError to report a 400 for a bad nickname, and because
    Python matches the first compatible except clause, an IllegalTransition was
    caught there and reported as 400 E_NICKNAME instead of 409 E_CONSENT. A
    client told "your nickname is invalid" when the real problem was "this round
    is already closed" will retry the wrong thing forever.

    Deriving from Exception keeps the two failure classes distinct so no ordering
    of except clauses can confuse them.
    """


@dataclass
class Session:
    """One in-flight game session.

    Held in memory for the duration of the game and mirrored to the ``sessions``
    table so an admin export and the leaderboard survive a restart. The in-memory
    copy is authoritative while the session is live; the DB is the record.
    """

    token: str
    pin: str
    uid_hex: str
    player_id: str | None
    nickname: str | None
    state: SessionState
    round_index: int = 0
    lives: int = STARTING_LIVES
    score: int = 0
    streak: int = 0
    created_mono: int = 0
    created_wall: str = ""
    expires_at_mono: int = 0
    clock_offset_ms: int = 0
    # Round bookkeeping, owned by the engine but stored here so there is one
    # object describing the whole session.
    current_item_id: str | None = None
    round_started_mono: int | None = None
    round_limit_ms: int = 0
    render_ack_mono: int | None = None
    answers_this_round: int = 0
    # Set at game over so the review page can be fetched with a separate,
    # longer-lived token.
    review_token: str | None = None
    review_expires_at_mono: int | None = None
    game_over_mono: int | None = None

    def is_expired(self, now_mono: int) -> bool:
        """True if the join window has closed.

        Only meaningful before a phone connects. Once the game is under way the
        session lives until it ends, so expiry is checked against the states
        where a dangling session would otherwise linger forever.
        """
        if self.state in (SessionState.AWAITING_PHONE, SessionState.CONSENT):
            return now_mono >= self.expires_at_mono
        return False

    def transition(self, next_state: SessionState) -> None:
        """Move to ``next_state``, or raise IllegalTransition."""
        if next_state == self.state:
            return
        allowed = _ALLOWED_TRANSITIONS.get(self.state, frozenset())
        if next_state not in allowed:
            raise IllegalTransition(
                f"cannot move session from {self.state.value} to {next_state.value}"
            )
        self.state = next_state


def _new_token() -> str:
    """128 bits of entropy as 32 hex characters."""
    return secrets.token_hex(16)


def _new_pin() -> str:
    """A 4-digit PIN, zero-padded.

    ``secrets.randbelow(10000)`` rather than ``randint``, because randbelow is
    uniform over the range and does not have the off-by-one ambiguity that
    randint's inclusive/exclusive bounds invite.
    """
    return f"{secrets.randbelow(10000):04d}"


class SessionStore:
    """Holds the single active session and persists it.

    A lock guards the in-memory slot. Socket.IO callbacks, the bridge polling
    thread and the REST handlers can all touch the session concurrently, and
    without a lock two taps arriving close together could both see "no active
    session" and both create one.
    """

    def __init__(self) -> None:
        self._active: Session | None = None
        self._lock = threading.RLock()

    # ── Queries ───────────────────────────────────────────────────────────

    @property
    def active(self) -> Session | None:
        with self._lock:
            return self._active

    def is_busy(self) -> bool:
        """True if a session exists that has not finished or expired."""
        with self._lock:
            if self._active is None:
                return False
            now = monotonic_ms()
            if self._active.is_expired(now):
                return False
            return self._active.state not in (
                SessionState.GAME_OVER,
                SessionState.REVIEW,
                SessionState.EXPIRED,
            )

    def get_by_token(self, token: str) -> Session | None:
        if not token:
            return None
        with self._lock:
            if self._active is not None and self._active.token == token:
                return self._active
        return None

    def get_by_pin(self, pin: str) -> Session | None:
        """Look up the active session by its 4-digit PIN.

        The PIN is the fallback join path for when scanning the QR code does not
        work. Four digits is only 10,000 possibilities, which is why /api/join is
        rate limited per IP and locks out after five wrong attempts.
        """
        if not pin:
            return None
        with self._lock:
            if self._active is not None and self._active.pin == pin:
                return self._active
        return None

    def get_by_review_token(self, review_token: str) -> Session | None:
        if not review_token:
            return None
        with self._lock:
            if (
                self._active is not None
                and self._active.review_token == review_token
            ):
                return self._active
        return None

    # ── Lifecycle ─────────────────────────────────────────────────────────

    def create_from_tap(
        self,
        uid_hex: str,
        *,
        nickname: str | None = None,
        clock_offset_ms: int = 0,
    ) -> Session | None:
        """Create a session for a card tap. Returns None if one is already active.

        The caller must check ``is_busy()`` first and signal BUSY on the MCU if
        this returns None. Returning None rather than raising keeps the "tap
        while busy" path a normal outcome rather than an error condition, which
        it is: it happens every time a second student taps mid-game.
        """
        with self._lock:
            now = monotonic_ms()
            if self._active is not None and not self._active.is_expired(now):
                if self._active.state not in (
                    SessionState.GAME_OVER,
                    SessionState.REVIEW,
                    SessionState.EXPIRED,
                ):
                    return None

            pid = compute_player_id(uid_hex, nickname) if nickname else None

            session = Session(
                token=_new_token(),
                pin=_new_pin(),
                uid_hex=uid_hex,
                player_id=pid,
                nickname=nickname,
                state=SessionState.AWAITING_PHONE,
                created_mono=now,
                created_wall=wall_now_iso(),
                expires_at_mono=now + int(SESSION_TTL_S * 1000),
                clock_offset_ms=clock_offset_ms,
            )
            self._active = session
            self._persist_new(session)
            return session

    def expire_if_needed(self) -> Session | None:
        """Expire the active session if its join window has closed.

        Called on every bridge poll and on every API request, so a dangling
        session cannot survive past its TTL even if nothing else looks at it.
        Returns the expired session, or None if nothing expired.
        """
        with self._lock:
            if self._active is None:
                return None
            now = monotonic_ms()
            if not self._active.is_expired(now):
                return None
            expired = self._active
            expired.state = SessionState.EXPIRED
            self._persist_state(expired)
            self._active = None
            return expired

    def clear(self) -> None:
        """Drop the active session without marking it expired.

        Used at game end, after the review window has closed, and by the admin
        reset. The DB row is left intact; only the in-memory slot is freed.
        """
        with self._lock:
            self._active = None

    # ── Persistence ───────────────────────────────────────────────────────

    def _persist_new(self, s: Session) -> None:
        # The raw UID is held in memory for the lifetime of the session because
        # computing player_id needs it, but it is NEVER written to the database.
        # card_ref is a keyed HMAC of it, so sessions can still be grouped by
        # physical card without the DB becoming a card tracker.
        ref = card_ref(s.uid_hex)
        with transaction() as tx:
            tx.execute(
                """INSERT INTO sessions
                   (token, pin, player_id, card_ref, state, round_index, lives,
                    score, streak, created_mono, created_wall, clock_offset_ms,
                    expires_at_mono)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                (
                    s.token, s.pin, s.player_id, ref, s.state.value,
                    s.round_index, s.lives, s.score, s.streak,
                    s.created_mono, s.created_wall, s.clock_offset_ms,
                    s.expires_at_mono,
                ),
            )
            tx.execute(
                """INSERT INTO tap_log (card_ref, player_id, session_token, t_mono, t_wall)
                   VALUES (?,?,?,?,?)""",
                (ref, s.player_id, s.token, s.created_mono, s.created_wall),
            )

    def _persist_state(self, s: Session) -> None:
        db().execute(
            """UPDATE sessions SET state=?, round_index=?, lives=?, score=?,
                   streak=?, game_over_mono=?, review_token=?,
                   review_expires_at_mono=?
               WHERE token=?""",
            (
                s.state.value, s.round_index, s.lives, s.score, s.streak,
                s.game_over_mono, s.review_token, s.review_expires_at_mono,
                s.token,
            ),
        )

    def persist(self, s: Session) -> None:
        """Public wrapper so the engine can save after mutating a session."""
        with self._lock:
            self._persist_state(s)
