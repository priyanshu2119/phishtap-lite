"""
PhishTap Lite — the game engine.

Owns the server-side session state machine from docs/ARCHITECTURE.md §5 and is
the single authority on timing, scoring and lives. The MCU renders; the phone
displays; this module decides.

THREADING
Called from three places at once: the bridge polling thread, Socket.IO callback
threads and REST handler threads. One re-entrant lock guards every mutation. An
RLock rather than a Lock because a public method may call another public method
(``submit_answer`` -> ``_end_game``), and a plain Lock would deadlock on the
second acquisition.

WHAT THE PHONE IS TOLD, AND WHEN
docs/ARCHITECTURE.md §6 says correct answers are not sent to the phone until the
game ends. Read literally that would make the game unplayable, because losing a
life is itself the signal that the answer was wrong. So the interpretation
implemented here is:

  * ``round_result`` carries ``correct`` — the player must know they lost a life,
    and the lives mechanic already reveals it.
  * ``round_result`` does NOT carry the item's label, cues, explanation or
    lesson. That teaching content appears only on the review page after the game
    ends.

The distinction matters because the review page is where the learning happens.
Revealing the cues mid-game would let a student pattern-match the hint rather
than read the message.
"""

from __future__ import annotations

import logging
import random
import threading
from dataclasses import dataclass, field
from typing import Any, Protocol

from .bridge_port import BridgeError, BridgePort, LinkDown
from .clock import monotonic_ms, wall_now_iso
from .config import (
    CARD_COOLDOWN_S,
    LINK_LOSS_S,
    REVIEW_TOKEN_TTL_S,
    STARTING_LIVES,
)
from .corpus import CorpusItem, item_to_payload
from .db import db, transaction
from .privacy import player_id as compute_player_id, redact_for_log, validate_nickname
from .scoring import ScoreBreakdown, round_limit_ms, score_answer
from .sessions import IllegalTransition, Session, SessionState, SessionStore

logger = logging.getLogger("phishtap.engine")

KIOSK_ROOM = "kiosk"


class Notifier(Protocol):
    """How the engine pushes updates to browsers.

    Kept as a Protocol so the engine has no dependency on the web layer, and so
    tests can pass a recorder that simply appends to a list.
    """

    def to_room(self, room: str, event: str, payload: dict[str, Any]) -> None: ...
    def to_kiosk(self, event: str, payload: dict[str, Any]) -> None: ...


class NullNotifier:
    """Discards every notification. The default when no web layer is attached."""

    def to_room(self, room: str, event: str, payload: dict[str, Any]) -> None:
        return None

    def to_kiosk(self, event: str, payload: dict[str, Any]) -> None:
        return None


@dataclass
class RoundRecord:
    """One answered round, kept in memory for the review page.

    The database is the durable record; this is the fast path for building the
    review payload without a query per round.
    """

    round_index: int
    item: CorpusItem
    answered: int | None          # LABEL_PHISH / LABEL_SAFE, or None on timeout
    correct: bool
    timed_out: bool
    limit_ms: int
    remaining_ms: int
    reaction_ms: int | None
    breakdown: ScoreBreakdown
    answer_source: str
    ack_missing: bool


class GameEngine:
    """Drives one session at a time, from card tap to review page."""

    def __init__(
        self,
        bridge: BridgePort,
        store: SessionStore,
        items: list[CorpusItem],
        notifier: Notifier | None = None,
        *,
        rng: random.Random | None = None,
    ) -> None:
        self._bridge = bridge
        self._store = store
        self._items = items
        self._notify = notifier or NullNotifier()
        # A caller-supplied generator makes session pools reproducible in tests.
        self._rng = rng or random.Random()
        self._lock = threading.RLock()

        self._cursor = 0                 # event ring cursor
        self._last_mcu_mono = monotonic_ms()
        self._link_ok = False
        self._mcu_state: dict[str, Any] = {}

        # Per-session working state, cleared when a session ends.
        self._pool: list[CorpusItem] = []
        self._pool_index = 0
        self._rounds: list[RoundRecord] = []

    # ── Link health ───────────────────────────────────────────────────────

    @property
    def link_ok(self) -> bool:
        with self._lock:
            return self._link_ok

    @property
    def mcu_status(self) -> dict[str, Any]:
        with self._lock:
            return dict(self._mcu_state)

    def seconds_since_mcu(self) -> float:
        with self._lock:
            return (monotonic_ms() - self._last_mcu_mono) / 1000.0

    # ── Polling ───────────────────────────────────────────────────────────

    def poll(self) -> None:
        """Drain MCU events and refresh status. Called about ten times a second.

        Every failure mode is contained: a bridge error marks the link down and
        returns rather than propagating, because this is called from a loop and an
        exception here would kill the polling thread and silently freeze the whole
        console.
        """
        try:
            events, oldest, overflow = self._bridge.get_events(self._cursor)
        except LinkDown as e:
            self._mark_link_down("get_events", e)
            return
        except BridgeError as e:
            logger.error("bridge rejected pt.get_events: %s", e)
            self._mark_link_down("get_events", e)
            return

        with self._lock:
            self._link_ok = True
            self._last_mcu_mono = monotonic_ms()

        if overflow:
            # The cursor fell behind the ring, so some events were dropped. Log
            # it loudly: a lost card_tap means a student tapped and nothing
            # happened, which from the outside looks like a broken reader.
            logger.warning(
                "event ring overflowed: cursor=%d oldest=%d, some events were "
                "dropped. Resyncing from oldest.",
                self._cursor, oldest,
            )

        for ev in events:
            seq = ev.get("seq")
            if isinstance(seq, int):
                self._cursor = max(self._cursor, seq)
            self._dispatch(ev)

        if events:
            try:
                self._bridge.ack_events(self._cursor)
            except BridgeError as e:
                # A failed ack is not fatal: the ring is bounded and the next
                # poll will re-read from the cursor. Log and carry on.
                logger.warning("pt.ack_events failed: %s", e)

    def refresh_status(self) -> dict[str, Any]:
        """Read pt.get_status and cache it for the kiosk."""
        try:
            status = self._bridge.get_status()
        except (BridgeError, LinkDown) as e:
            self._mark_link_down("get_status", e)
            with self._lock:
                return dict(self._mcu_state)
        with self._lock:
            self._link_ok = True
            self._last_mcu_mono = monotonic_ms()
            self._mcu_state = status
        return status

    def _mark_link_down(self, where: str, exc: BaseException) -> None:
        with self._lock:
            was_ok = self._link_ok
            self._link_ok = False
        if was_ok:
            logger.error("MCU link lost during %s: %s", where, exc)
            self._notify.to_kiosk("hw_fault", {"code": "E_BRIDGE", "where": where})

    # ── Event dispatch ────────────────────────────────────────────────────

    def _dispatch(self, ev: dict[str, Any]) -> None:
        etype = ev.get("type")
        data = ev.get("data") or {}

        if etype == "card_tap":
            self._on_tap(data)
        elif etype == "card_removed":
            self._on_removal()
        elif etype == "round_timeout":
            self._on_timeout(data)
        elif etype == "hw_fault":
            code = data.get("code", "E_INTERNAL")
            # PT_FAULT_NONE ("E_NONE") is the firmware's recovery notice, not a
            # fault. Treating it as one would show a hardware-fault banner every
            # time the reader recovered, which trains everyone to ignore it.
            if code == "E_NONE":
                logger.info("MCU reported recovery")
            else:
                logger.error("MCU hardware fault: %s", code)
                self._notify.to_kiosk("hw_fault", {"code": code})
        elif etype == "heartbeat":
            # Liveness is already recorded by the successful poll; nothing else
            # to do. Handled explicitly so an unknown type is the only thing
            # that falls through to the warning below.
            pass
        elif etype == "button":
            # Stage 6. The event type exists in the firmware already; nothing
            # drives it yet.
            logger.debug("button event received (Stage 6, not wired): %s", data)
        else:
            logger.warning("unknown event type from MCU: %r", etype)

    def _on_tap(self, data: dict[str, Any]) -> None:
        # The UID is privacy-sensitive. Log the redacted form so the fact that a
        # tap happened is visible without the identifier reaching a log file.
        logger.info("card tap: %s", redact_for_log(dict(data)))

        uid_hex = str(data.get("uid_hex", ""))
        if not uid_hex:
            logger.error("card_tap event had no uid_hex, ignoring")
            return

        cmds: list[tuple[Any, ...]] = []
        pin: str | None = None

        with self._lock:
            # Expire a stale session first, so a tap arriving just after a TTL
            # lapse is not wrongly refused as busy.
            self._store.expire_if_needed()

            if self._store.is_busy():
                cmds.append(("buzz", ("busy",)))
                cmds.append(("set_state", ("busy",)))
            else:
                session = self._store.create_from_tap(uid_hex)
                if session is None:
                    cmds.append(("buzz", ("busy",)))
                    cmds.append(("set_state", ("busy",)))
                else:
                    self._pool = []
                    self._pool_index = 0
                    self._rounds = []
                    pin = session.pin
                    cmds.append(("set_state", ("tapped",)))
                    cmds.append(("buzz", ("welcome",)))

        # Bridge calls happen here, with no lock held.
        self._run_cmds(cmds)

        if pin is not None:
            # The PIN is not secret in the way a token is: it is displayed on the
            # kiosk screen for anyone in the room to read, and it only joins a
            # session that expires in 90 seconds. Logging it is what makes a demo
            # recoverable when the QR code will not scan.
            logger.info("session created, pin=%s", pin)
            self._notify.to_kiosk("session_started", self.kiosk_snapshot())

    def _on_removal(self) -> None:
        logger.debug("card removed")
        self._notify.to_kiosk("kiosk_state", self.kiosk_snapshot())

    def _on_timeout(self, data: dict[str, Any]) -> None:
        """The MCU's countdown hit zero before an answer arrived.

        The server also grades timeouts from its own deadline, so this can arrive
        for a round the server has already closed. Ignoring a timeout for a round
        that is no longer awaiting an answer is what stops a race between the two
        clocks from costing the player two lives for one slow answer.
        """
        with self._lock:
            session = self._store.active
            if session is None:
                return
            if session.state != SessionState.AWAITING_ANSWER:
                logger.debug(
                    "ignoring MCU timeout for round %s, server state is %s",
                    data.get("round"), session.state.value,
                )
                return
        self._grade(session_token=None, answer=None, source="timeout")

    # ── Phone-side flow ───────────────────────────────────────────────────

    def connect_phone(self, token: str) -> dict[str, Any]:
        """A phone presented a token. Returns the join payload.

        Raises KeyError if the token is unknown or expired, which the API layer
        turns into a 404. The caller rate limits before getting here.
        """
        cmds: list[tuple[Any, ...]] = []

        with self._lock:
            self._store.expire_if_needed()
            session = self._store.get_by_token(token)
            if session is None:
                raise KeyError("unknown or expired session token")

            if session.state == SessionState.AWAITING_PHONE:
                # A returning player skips consent; a new one does not. The
                # player_id is only known once a nickname exists, so look the
                # player up by the card UID in shared-card mode.
                existing = self._find_existing_player(session)
                if existing is None:
                    session.transition(SessionState.CONSENT)
                else:
                    session.player_id = existing["player_id"]
                    session.nickname = existing["nickname"]
                    session.transition(SessionState.ROUND_START)
                    self._start_first_round(session, cmds)
                self._store.persist(session)

            payload = {
                "token": session.token,
                "state": session.state.value,
                "nickname": session.nickname,
                "needs_consent": session.state == SessionState.CONSENT,
                "lives": session.lives,
                "score": session.score,
            }

        self._run_cmds(cmds)
        return payload

    def submit_consent(self, token: str, nickname: str, consent_version: int) -> dict[str, Any]:
        """Record consent and a nickname, then start the game."""
        ok, result = validate_nickname(nickname)
        if not ok:
            raise ValueError(result)

        cmds: list[tuple[Any, ...]] = []

        with self._lock:
            session = self._store.get_by_token(token)
            if session is None:
                raise KeyError("unknown or expired session token")
            if session.state not in (SessionState.CONSENT, SessionState.AWAITING_PHONE):
                raise IllegalTransition(
                    f"consent cannot be submitted in state {session.state.value}"
                )

            session.nickname = result
            session.player_id = compute_player_id(session.uid_hex, result)
            self._upsert_player(session, consent_version)

            if session.state == SessionState.AWAITING_PHONE:
                session.transition(SessionState.CONSENT)
            session.transition(SessionState.ROUND_START)
            self._start_first_round(session, cmds)
            self._store.persist(session)
            pid = session.player_id

        self._run_cmds(cmds)
        return {"ok": True, "nickname": result, "player_id": pid}

    def render_ack(self, token: str) -> None:
        """The phone has painted the message; start the reaction-time clock.

        Reaction time is measured from here rather than from round_start, because
        the interval between the two is network and render latency that has
        nothing to do with how fast the student read the message.
        """
        with self._lock:
            session = self._store.get_by_token(token)
            if session is None or session.state != SessionState.AWAITING_ANSWER:
                return
            session.render_ack_mono = monotonic_ms()

    def submit_answer(
        self, token: str, answer: str, *, source: str = "phone"
    ) -> dict[str, Any]:
        """Grade an answer. ``answer`` is "phish" or "safe".

        Raises KeyError for an unknown token, ValueError for a malformed answer,
        and IllegalTransition if the round is not awaiting one. The API layer maps
        each to an HTTP status.
        """
        if answer not in ("phish", "safe"):
            raise ValueError("answer must be 'phish' or 'safe'")

        with self._lock:
            session = self._store.get_by_token(token)
            if session is None:
                raise KeyError("unknown or expired session token")
            if session.state != SessionState.AWAITING_ANSWER:
                raise IllegalTransition(
                    f"not awaiting an answer (state is {session.state.value})"
                )
            if session.answers_this_round >= 3:
                # Three attempts per round, per the API spec. Refusing rather than
                # ignoring, so a client that retries in a loop gets told.
                raise ValueError("too many answers for this round")

        return self._grade(session_token=token, answer=answer, source=source)

    # ── Round mechanics ───────────────────────────────────────────────────

    def _start_first_round(self, session: Session, cmds: list[tuple[Any, ...]]) -> None:
        """Build the item pool and start round 0. Caller holds the lock."""
        from .corpus import build_session_pool

        self._pool = build_session_pool(self._items, self._rng)
        self._pool_index = 0
        self._rounds = []
        session.round_index = 0
        session.lives = STARTING_LIVES
        session.score = 0
        session.streak = 0
        self._begin_round(session, cmds)

    def _begin_round(self, session: Session, cmds: list[tuple[Any, ...]]) -> None:
        """Arm one round. Caller holds the lock and supplies the command list.

        Bridge calls are APPENDED to ``cmds`` rather than made here. Every
        ``Bridge.call`` can block for up to BRIDGE_CALL_TIMEOUT_S, and doing that
        while holding the engine lock would stall every other thread — the poll
        loop, the REST handlers and the Socket.IO callbacks — for the duration. A
        hung MCU would freeze the whole backend instead of just the console.
        """
        if self._pool_index >= len(self._pool):
            self._end_game(session, reason="pool_exhausted", cmds=cmds)
            return

        item = self._pool[self._pool_index]
        limit = round_limit_ms(session.round_index)
        now = monotonic_ms()

        session.current_item_id = item.id
        session.round_started_mono = now
        session.round_limit_ms = limit
        session.render_ack_mono = None
        session.answers_this_round = 0
        session.transition(SessionState.AWAITING_ANSWER)
        self._store.persist(session)

        # Arm the MCU's countdown before telling the phone, so the two clocks
        # start as close together as possible.
        cmds.append(("start_round", (session.round_index, limit)))
        cmds.append(("set_state", ("playing",)))

        deadline_ms = now + limit
        self._notify.to_room(
            session.token,
            "round_start",
            {
                "round": session.round_index,
                "limit_ms": limit,
                "deadline_mono": deadline_ms,
                "lives": session.lives,
                "score": session.score,
                "item": item_to_payload(item, include_answer=False),
            },
        )
        self._notify.to_kiosk("round_start", self.kiosk_snapshot())

    def _run_cmds(self, cmds: list[tuple[Any, ...]]) -> None:
        """Execute deferred MCU commands. Called with NO lock held.

        Each failure is logged and skipped rather than aborting the rest: if the
        buzzer call fails there is no reason not to still show the score. The game
        on the phone keeps working regardless, because losing the physical
        feedback is a degraded experience while losing the game is a broken one.
        """
        for entry in cmds:
            name, args = entry
            try:
                getattr(self._bridge, name)(*args)
            except BridgeError as e:
                logger.warning("MCU call %s%s failed: %s", name, args, e)
            except Exception:  # noqa: BLE001
                # A bug in this backend must not take down the polling thread.
                logger.exception("unexpected error issuing MCU call %s", name)

    def _grade(self, *, session_token: str | None, answer: str | None, source: str) -> dict[str, Any]:
        """Grade the current round and advance. ``answer`` is None on a timeout."""
        cmds: list[tuple[Any, ...]] = []

        with self._lock:
            session = (
                self._store.get_by_token(session_token)
                if session_token
                else self._store.active
            )
            if session is None or session.state != SessionState.AWAITING_ANSWER:
                return {"ok": False, "reason": "no round awaiting an answer"}

            now = monotonic_ms()
            timed_out = answer is None

            item = self._current_item(session)
            if item is None:
                logger.error("round %d has no current item, ending game", session.round_index)
                self._end_game(session, reason="no_item", cmds=cmds)
                snapshot = self.kiosk_snapshot()
            else:
                deadline = (session.round_started_mono or now) + session.round_limit_ms
                remaining = max(0, deadline - now)

                # Reaction time, per docs/ARCHITECTURE.md §5: measured from the
                # phone's render ack, falling back to round_start with a flag if
                # no ack arrived within 2 s. The flag keeps the analytics honest
                # rather than silently mixing two different measurements into one
                # distribution.
                ack = session.render_ack_mono
                ack_missing = False
                if ack is None:
                    ack = session.round_started_mono
                    ack_missing = True
                elif ack - (session.round_started_mono or ack) > 2000:
                    ack = session.round_started_mono
                    ack_missing = True
                reaction = (now - ack) if ack is not None else None

                answered_label = None if timed_out else (1 if answer == "phish" else 0)
                correct = (not timed_out) and answered_label == item.label

                breakdown = score_answer(
                    correct=correct,
                    remaining_ms=remaining,
                    limit_ms=session.round_limit_ms,
                    streak_before=session.streak,
                )

                session.answers_this_round += 1
                if correct:
                    session.streak += 1
                    session.score += breakdown.points
                else:
                    session.streak = 0
                    session.lives = max(0, session.lives - 1)

                self._rounds.append(
                    RoundRecord(
                        round_index=session.round_index,
                        item=item,
                        answered=answered_label,
                        correct=correct,
                        timed_out=timed_out,
                        limit_ms=session.round_limit_ms,
                        remaining_ms=remaining,
                        reaction_ms=reaction,
                        breakdown=breakdown,
                        answer_source=source,
                        ack_missing=ack_missing,
                    )
                )
                self._persist_answer(session, item, answered_label, correct, timed_out,
                                     remaining, reaction, breakdown, source, ack_missing)

                session.transition(SessionState.ROUND_RESULT)
                self._store.persist(session)

                # Snapshot everything the response and the notifications need
                # while the lock is still held, so nothing can change underneath
                # between releasing it and reading these values.
                lives = session.lives
                score = session.score
                streak = session.streak
                round_index = session.round_index
                token = session.token

                cmds.append((
                    "end_round",
                    ("correct" if correct else ("timeout" if timed_out else "wrong"),),
                ))
                cmds.append(("set_lives", (lives,)))
                cmds.append(("buzz", ("ok" if correct else "wrong",)))

                result_payload = {
                    "round": round_index,
                    "correct": correct,
                    "timed_out": timed_out,
                    "points": breakdown.points,
                    "speed": breakdown.speed,
                    "multiplier": breakdown.multiplier,
                    "streak": streak,
                    "lives": lives,
                    "score": score,
                    "reaction_ms": reaction,
                }

                if lives <= 0:
                    self._end_game(session, reason="no_lives", cmds=cmds)
                elif self._pool_index + 1 >= len(self._pool):
                    self._end_game(session, reason="pool_exhausted", cmds=cmds)
                else:
                    self._pool_index += 1
                    session.round_index += 1
                    session.transition(SessionState.ROUND_START)
                    self._begin_round(session, cmds)

                snapshot = self.kiosk_snapshot()

        # Everything below runs with NO lock held, so the blocking bridge calls
        # cannot stall another thread.
        self._run_cmds(cmds)

        if item is None:
            self._notify.to_kiosk("round_result", snapshot)
            return {"ok": False, "reason": "no current item"}

        # The result tells the player whether they lost a life, which the lives
        # mechanic reveals anyway. It deliberately omits the item's label, cues
        # and explanation: those are the teaching content and belong on the
        # review page, not mid-game.
        self._notify.to_room(token, "round_result", result_payload)
        self._notify.to_kiosk("round_result", snapshot)
        return {"ok": True, **result_payload}

    def _current_item(self, session: Session) -> CorpusItem | None:
        if self._pool_index >= len(self._pool):
            return None
        item = self._pool[self._pool_index]
        # Belt and braces: the session records which item it believes is live, so
        # a desync between the two is detectable rather than silently grading the
        # wrong message.
        if session.current_item_id is not None and session.current_item_id != item.id:
            logger.error(
                "session %s expects item %s but the pool is at %s",
                session.token[:8], session.current_item_id, item.id,
            )
        return item

    def _end_game(
        self, session: Session, *, reason: str, cmds: list[tuple[Any, ...]]
    ) -> None:
        """Finish the game and open the review window. Caller holds the lock.

        Bridge calls are appended to ``cmds`` and executed by the caller after it
        releases the lock, for the same reason as in ``_begin_round``: a blocking
        call under the engine lock would stall every other thread.
        """
        import secrets

        now = monotonic_ms()
        session.game_over_mono = now
        session.review_token = secrets.token_hex(16)
        session.review_expires_at_mono = now + int(REVIEW_TOKEN_TTL_S * 1000)
        try:
            session.transition(SessionState.GAME_OVER)
        except IllegalTransition:
            # Already terminal; force it so a game can always be closed even if
            # the state machine got somewhere unexpected. Leaving a session stuck
            # would block every subsequent tap.
            session.state = SessionState.GAME_OVER
        self._store.persist(session)
        logger.info("game over (%s): score=%d rounds=%d", reason, session.score, len(self._rounds))

        cmds.append(("set_state", ("over",)))
        cmds.append(("show_score", (session.score,)))
        cmds.append(("buzz", ("over",)))

        self._notify.to_room(
            session.token,
            "game_over",
            {
                "score": session.score,
                "rounds_played": len(self._rounds),
                "reason": reason,
                "review_token": session.review_token,
            },
        )
        self._notify.to_kiosk("game_over", self.kiosk_snapshot())

    # ── Persistence helpers ───────────────────────────────────────────────

    def _find_existing_player(self, session: Session) -> dict[str, Any] | None:
        """Look up a player who has already given consent on this card.

        In shared-card mode the player_id depends on the nickname, which is not
        known until the phone submits it, so the lookup is by the card and returns
        the most recent player. That is a heuristic and it is why the consent
        screen is shown whenever the lookup finds nothing.

        The join is on ``card_ref``, the keyed HMAC of the UID, never on the UID
        itself: the raw identifier is not in the database at all.
        """
        from .privacy import card_ref

        row = db().execute(
            """SELECT player_id, nickname FROM players
               WHERE deleted = 0
                 AND player_id IN (
                     SELECT DISTINCT player_id FROM sessions
                     WHERE card_ref = ? AND player_id IS NOT NULL
                 )
               ORDER BY created_mono DESC LIMIT 1""",
            (card_ref(session.uid_hex),),
        ).fetchone()
        if row is None:
            return None
        return {"player_id": row["player_id"], "nickname": row["nickname"]}

    def _upsert_player(self, session: Session, consent_version: int) -> None:
        now_mono = monotonic_ms()
        now_wall = wall_now_iso()
        db().execute(
            """INSERT INTO players
               (player_id, nickname, consent_version, consent_at_mono,
                consent_at_wall, created_mono, created_wall, deleted)
               VALUES (?,?,?,?,?,?,?,0)
               ON CONFLICT(player_id) DO UPDATE SET
                 nickname=excluded.nickname,
                 consent_version=excluded.consent_version,
                 consent_at_mono=excluded.consent_at_mono,
                 consent_at_wall=excluded.consent_at_wall,
                 deleted=0""",
            (
                session.player_id, session.nickname, consent_version,
                now_mono, now_wall, now_mono, now_wall,
            ),
        )
        # The session was created before the nickname was known, so its
        # player_id is still NULL. Backfill it so the foreign key and the
        # analytics joins work.
        db().execute(
            "UPDATE sessions SET player_id=? WHERE token=?",
            (session.player_id, session.token),
        )

    def _persist_answer(
        self,
        session: Session,
        item: CorpusItem,
        answered_label: int | None,
        correct: bool,
        timed_out: bool,
        remaining_ms: int,
        reaction_ms: int | None,
        breakdown: ScoreBreakdown,
        source: str,
        ack_missing: bool,
    ) -> None:
        db().execute(
            """INSERT INTO answers
               (session_token, player_id, round_index, item_id, label_correct,
                label_answered, correct, timed_out, limit_ms, remaining_ms,
                reaction_ms, streak_before, speed, multiplier, points,
                answer_source, ack_missing, answered_mono, answered_wall)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (
                session.token, session.player_id, session.round_index, item.id,
                item.label, answered_label if answered_label is not None else -1,
                1 if correct else 0, 1 if timed_out else 0,
                session.round_limit_ms, remaining_ms, reaction_ms,
                breakdown.streak_before, breakdown.speed, breakdown.multiplier,
                breakdown.points, source, 1 if ack_missing else 0,
                monotonic_ms(), wall_now_iso(),
            ),
        )

    # ── Snapshots ─────────────────────────────────────────────────────────
    #
    # Note on locking: these are called from inside lock-holding sections, and
    # they re-acquire the same RLock. That is safe because it is re-entrant and
    # always the same thread. They also call the notifier, which schedules a
    # Socket.IO emit rather than blocking on one, and run at most one indexed
    # SQLite query for the leaderboard. Holding the lock across those is a
    # deliberate trade: the alternative is snapshotting, releasing, and then
    # publishing a state that another thread has already changed, which is a
    # worse bug than a few milliseconds of contention.

    def kiosk_snapshot(self) -> dict[str, Any]:
        """Everything the kiosk page needs, in one payload.

        Includes the MCU status so the kiosk can show a hardware-fault banner
        without a second request, and the session URL and PIN so the QR code can
        be rendered client-side.
        """
        with self._lock:
            session = self._store.active
            mcu = dict(self._mcu_state)
            link_ok = self._link_ok
            seconds = (monotonic_ms() - self._last_mcu_mono) / 1000.0

        return {
            "link_ok": link_ok and seconds < LINK_LOSS_S,
            "seconds_since_mcu": round(seconds, 1),
            "mcu": mcu,
            "session": None if session is None else {
                "token": session.token,
                "pin": session.pin,
                "state": session.state.value,
                "round": session.round_index,
                "lives": session.lives,
                "score": session.score,
                "streak": session.streak,
                "nickname": session.nickname,
            },
            "leaderboard": self.leaderboard(),
        }

    def session_payload(self, token: str) -> dict[str, Any]:
        """What the phone needs to resume or render its current round."""
        with self._lock:
            session = self._store.get_by_token(token)
            if session is None:
                raise KeyError("unknown or expired session token")

            payload: dict[str, Any] = {
                "token": session.token,
                "state": session.state.value,
                "nickname": session.nickname,
                "lives": session.lives,
                "score": session.score,
                "streak": session.streak,
                "round": session.round_index,
            }
            if (
                session.state == SessionState.AWAITING_ANSWER
                and self._pool_index < len(self._pool)
                and session.round_started_mono is not None
            ):
                item = self._pool[self._pool_index]
                payload["current"] = {
                    "round": session.round_index,
                    "limit_ms": session.round_limit_ms,
                    "deadline_mono": session.round_started_mono + session.round_limit_ms,
                    "item": item_to_payload(item, include_answer=False),
                }
            return payload

    def review_payload(self, review_token: str) -> dict[str, Any]:
        """The post-game review: every round, with the answers revealed.

        This is the only place the correct labels, cues and explanations are ever
        sent to a phone.
        """
        with self._lock:
            session = self._store.get_by_review_token(review_token)
            if session is None:
                raise KeyError("unknown review token")
            now = monotonic_ms()
            if (
                session.review_expires_at_mono is not None
                and now >= session.review_expires_at_mono
            ):
                raise KeyError("review token expired")

            rounds = [
                {
                    "round": r.round_index,
                    "channel": r.item.channel,
                    "sender": r.item.sender,
                    "subject": r.item.subject,
                    "body": r.item.body,
                    "correct_label": r.item.label,
                    "your_answer": r.answered,
                    "you_were_right": r.correct,
                    "timed_out": r.timed_out,
                    "cues": list(r.item.cues),
                    "explanation": r.item.explanation,
                    "lesson": r.item.lesson,
                    "points": r.breakdown.points,
                    "reaction_ms": r.reaction_ms,
                }
                for r in self._rounds
            ]
            return {
                "score": session.score,
                "rounds_played": len(self._rounds),
                "nickname": session.nickname,
                "player_id": session.player_id,
                "rounds": rounds,
            }

    def leaderboard(self, limit: int = 10) -> list[dict[str, Any]]:
        """Top scores across all players who have not been deleted."""
        rows = db().execute(
            """SELECT p.nickname, MAX(s.score) AS best, COUNT(DISTINCT s.token) AS games
               FROM players p
               JOIN sessions s ON s.player_id = p.player_id
               WHERE p.deleted = 0 AND s.game_over_mono IS NOT NULL
               GROUP BY p.player_id
               ORDER BY best DESC
               LIMIT ?""",
            (limit,),
        ).fetchall()
        return [
            {"nickname": r["nickname"], "score": r["best"], "games": r["games"]}
            for r in rows
        ]
