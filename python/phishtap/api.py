"""
PhishTap Lite — API service layer.

Every REST and WebSocket handler's LOGIC lives here as a plain method that takes
ordinary arguments and returns ``(status_code, payload)``. No FastAPI, no
Socket.IO, no ``arduino.*``.

WHY THE SPLIT
The obvious design is to write FastAPI handlers directly. That works, but it
makes the game's entire HTTP surface untestable without a web server, and it
tangles three concerns: extracting the request, applying policy, and doing the
work. Splitting them means:

  * this module is pure Python and runs under plain pytest on a laptop
  * ``adapters/web_adapter.py`` is a thin shell that only extracts request data
    and formats the response, which is the part that genuinely needs FastAPI
  * a policy change (a rate limit, a new validation) is made once, here, and is
    covered by a test

The adapter is the only place that knows what a ``Request`` object is.

CONVENTION
Every method returns ``ApiResult = tuple[int, dict]``. The status code is chosen
here rather than by raising HTTP exceptions, so the mapping from failure to
status is visible in one file and testable without an HTTP layer.
"""

from __future__ import annotations

import csv
import io
import logging
import secrets
import threading
from typing import Any

from .clock import ClockOffset, monotonic_ms
from .config import (
    DB_RESET_CONFIRM,
    ADMIN_PIN_LENGTH,
    ADMIN_PIN_PATH,
    FW_VERSION,
    JOIN_LOCKOUT_THRESHOLD,
    PROTO_VERSION,
    RATE_ADMIN_LOGIN_PER_MIN,
    RATE_ANSWER_PER_ROUND,
    RATE_CONNECT_PER_MIN,
    RATE_JOIN_PER_MIN,
)
from .db import db
from .engine import GameEngine
from .ratelimit import JoinAttempts, RateLimiter
from .sessions import IllegalTransition

logger = logging.getLogger("phishtap.api")

ApiResult = tuple[int, dict[str, Any]]

# How long an admin login stays valid. Short enough that a shared demo laptop
# left open is not an open door for long, long enough to do an export and a
# deletion without re-authenticating.
ADMIN_SESSION_TTL_MS = 30 * 60 * 1000


def _err(status: int, code: str, message: str = "") -> ApiResult:
    """Build an error response. ``code`` is one of the E_* codes from §4."""
    payload: dict[str, Any] = {"ok": False, "err": code}
    if message:
        payload["message"] = message
    return status, payload


def _ok(payload: dict[str, Any] | None = None, status: int = 200) -> ApiResult:
    body = {"ok": True}
    if payload:
        body.update(payload)
    return status, body


class AdminAuth:
    """Admin PIN storage and short-lived session tokens.

    The PIN is generated on first use and stored in ``data/admin_pin``. It is
    stored in CLEAR TEXT, and that is a deliberate trade worth stating: the file
    is mode 0600 inside a container on a board that a demonstrator physically
    controls, and hashing it would only protect against someone who already has
    read access to the file — at which point they can also read the database and
    the server secret. Hashing would add a recovery problem (a forgotten PIN with
    no reset path) without adding a real barrier.

    Session tokens are random and expire. They are held in memory only, so an app
    restart logs the admin out, which is the correct failure mode.
    """

    def __init__(self, pin_path=ADMIN_PIN_PATH) -> None:
        self._pin_path = pin_path
        self._sessions: dict[str, int] = {}   # token -> expiry (monotonic ms)
        self._lock = threading.Lock()

    def get_or_create_pin(self) -> str:
        if self._pin_path.exists():
            stored = self._pin_path.read_text(encoding="utf-8").strip()
            if stored:
                return stored
        from .config import ensure_data_dir
        ensure_data_dir()
        pin = "".join(secrets.choice("0123456789") for _ in range(ADMIN_PIN_LENGTH))
        self._pin_path.write_text(pin + "\n", encoding="utf-8")
        try:
            self._pin_path.chmod(0o600)
        except OSError:
            pass
        return pin

    def verify(self, pin: str) -> str | None:
        """Check a PIN. Returns a session token on success, None on failure.

        Compared with ``secrets.compare_digest`` rather than ``==`` so the
        comparison time does not depend on how many leading digits matched. A
        six-digit PIN over a local network is not a realistic timing-attack
        target, but using the constant-time comparison costs nothing and removes
        the question entirely.
        """
        expected = self.get_or_create_pin()
        if secrets.compare_digest(str(pin or ""), expected):
            token = secrets.token_hex(16)
            with self._lock:
                self._sessions[token] = monotonic_ms() + ADMIN_SESSION_TTL_MS
            return token
        return None

    def check(self, token: str | None) -> bool:
        if not token:
            return False
        now = monotonic_ms()
        with self._lock:
            expiry = self._sessions.get(token)
            if expiry is None:
                return False
            if now >= expiry:
                del self._sessions[token]
                return False
            return True

    def revoke(self, token: str) -> None:
        with self._lock:
            self._sessions.pop(token, None)


class ApiService:
    """All HTTP-facing policy and logic. See the module docstring for why."""

    def __init__(
        self,
        engine: GameEngine,
        *,
        limiter: RateLimiter | None = None,
        join_attempts: JoinAttempts | None = None,
        admin: AdminAuth | None = None,
        clock: ClockOffset | None = None,
        base_url: str = "",
    ) -> None:
        self.engine = engine
        self.limiter = limiter or RateLimiter()
        self.join_attempts = join_attempts or JoinAttempts(JOIN_LOCKOUT_THRESHOLD)
        self.admin = admin or AdminAuth()
        self.clock = clock or ClockOffset()
        self._base_url = base_url
        self._started_mono = monotonic_ms()

    @property
    def base_url(self) -> str:
        return self._base_url

    @base_url.setter
    def base_url(self, value: str) -> None:
        self._base_url = value or ""

    # ── Public, unauthenticated ───────────────────────────────────────────

    def health(self) -> ApiResult:
        """Liveness and version. Deliberately reveals nothing about players."""
        return _ok({
            "version": FW_VERSION,
            "proto": PROTO_VERSION,
            "base_url": self._base_url,
            "uptime_s": round((monotonic_ms() - self._started_mono) / 1000.0, 1),
            "bridge_ok": self.engine.link_ok,
        })

    def kiosk(self) -> ApiResult:
        """Everything the kiosk screen renders, in one call."""
        snap = self.engine.kiosk_snapshot()
        snap["base_url"] = self._base_url
        session = snap.get("session")
        if session and session.get("token") and self._base_url:
            # The QR is generated client-side from this URL. The kiosk also shows
            # it as text, because DHCP can change the board's address and a QR
            # that cannot be scanned should still be typeable.
            session["url"] = f"{self._base_url}/game.html?token={session['token']}"
        return _ok(snap)

    def get_session(self, token: str) -> ApiResult:
        if not token:
            return _err(400, "E_TOKEN", "token is required")
        try:
            return _ok(self.engine.session_payload(token))
        except KeyError:
            return _err(404, "E_EXPIRED", "unknown or expired session")

    def connect(self, token: str, ip: str) -> ApiResult:
        """A phone joined by scanning the QR code."""
        if not self.limiter.allow(
            f"connect:{ip}", capacity=RATE_CONNECT_PER_MIN, refill_per_min=RATE_CONNECT_PER_MIN
        ):
            return _err(429, "E_RATE", "too many attempts, wait a moment")
        if not token:
            return _err(400, "E_TOKEN", "token is required")
        try:
            return _ok(self.engine.connect_phone(token))
        except KeyError:
            return _err(404, "E_EXPIRED", "unknown or expired session")

    def join(self, pin: str, ip: str) -> ApiResult:
        """PIN fallback for when the QR code will not scan.

        Rate limited AND locked out after five wrong attempts, because a 4-digit
        PIN is only 10,000 possibilities and anyone on the same Wi-Fi could brute
        force it. The lockout is per IP and clears on a success.
        """
        if not self.limiter.allow(
            f"join:{ip}", capacity=RATE_JOIN_PER_MIN, refill_per_min=RATE_JOIN_PER_MIN
        ):
            return _err(429, "E_RATE", "too many attempts, wait a moment")

        key = f"joinlock:{ip}"
        if self.join_attempts.is_locked(key):
            return _err(423, "E_PIN", "too many wrong PINs, wait for the current session to end")

        if not pin:
            return _err(400, "E_PIN", "pin is required")

        session = self.engine.store.get_by_pin(pin)
        if session is None:
            locked = self.join_attempts.record_failure(key)
            remaining = self.join_attempts.remaining(key)
            return _err(
                404, "E_PIN",
                "locked out" if locked else f"wrong PIN, {remaining} attempt(s) left",
            )

        self.join_attempts.record_success(key)
        try:
            return _ok(self.engine.connect_phone(session.token))
        except KeyError:
            return _err(404, "E_EXPIRED", "that session just expired")

    def consent(self, token: str, nickname: str, consent_version: int, ip: str) -> ApiResult:
        if not self.limiter.allow(
            f"consent:{ip}", capacity=RATE_CONNECT_PER_MIN, refill_per_min=RATE_CONNECT_PER_MIN
        ):
            return _err(429, "E_RATE", "too many attempts, wait a moment")
        if not token:
            return _err(400, "E_TOKEN", "token is required")
        try:
            return _ok(self.engine.submit_consent(token, nickname, consent_version))
        except KeyError:
            return _err(404, "E_EXPIRED", "unknown or expired session")
        except IllegalTransition as e:
            # Most specific first. IllegalTransition is no longer a ValueError so
            # the order cannot bite, but keeping the narrow clause above the broad
            # one is what stops the next person reintroducing the bug.
            return _err(409, "E_CONSENT", str(e))
        except ValueError as e:
            return _err(400, "E_NICKNAME", str(e))

    def render_ack(self, token: str) -> ApiResult:
        """The phone has painted the message; the reaction clock starts here."""
        if not token:
            return _err(400, "E_TOKEN", "token is required")
        self.engine.render_ack(token)
        # Always 200. An ack for a round that has already ended is harmless and
        # common (a slow phone, a reconnect), and failing it would make the client
        # retry into a loop.
        return _ok()

    def answer(self, token: str, answer: str, source: str, ip: str) -> ApiResult:
        if not token:
            return _err(400, "E_TOKEN", "token is required")
        if answer not in ("phish", "safe"):
            return _err(400, "E_INTERNAL", "answer must be 'phish' or 'safe'")

        # Per-round answer cap. Checked before the engine call so the limit is
        # enforced even for a client that ignores the round_result it was sent.
        session = self.engine.store.get_by_token(token)
        if session is not None and session.answers_this_round >= RATE_ANSWER_PER_ROUND:
            return _err(429, "E_RATE", "too many answers for this round")

        try:
            result = self.engine.submit_answer(token, answer, source=source)
        except KeyError:
            return _err(404, "E_EXPIRED", "unknown or expired session")
        except IllegalTransition as e:
            # The round already closed, usually because the deadline passed while
            # the answer was in flight. 409 tells the client to stop and refresh
            # rather than retry.
            return _err(409, "E_EXPIRED", str(e))
        except ValueError as e:
            return _err(400, "E_INTERNAL", str(e))

        if not result.get("ok"):
            return _err(409, "E_EXPIRED", str(result.get("reason", "round closed")))
        return _ok(result)

    def review(self, token: str) -> ApiResult:
        """The post-game review. Valid for REVIEW_TOKEN_TTL_S after game over."""
        if not token:
            return _err(400, "E_TOKEN", "review token is required")
        try:
            return _ok(self.engine.review_payload(token))
        except KeyError:
            return _err(404, "E_EXPIRED", "unknown or expired review token")

    def leaderboard(self) -> ApiResult:
        return _ok({"leaderboard": self.engine.leaderboard()})

    def delete_request(self, token: str) -> ApiResult:
        """Player-facing delete-my-data, scoped to their own review token.

        Deliberately NOT behind a card tap: in shared-card mode a tap cannot say
        WHO is asking, so it would let one student erase another's data. The
        review token identifies exactly one player and only exists for ten
        minutes after their game.
        """
        if not token:
            return _err(400, "E_TOKEN", "review token is required")
        try:
            payload = self.engine.review_payload(token)
        except KeyError:
            return _err(404, "E_EXPIRED", "unknown or expired review token")

        player_id = payload.get("player_id")
        if not player_id:
            return _err(404, "E_INTERNAL", "no player attached to this session")
        self._delete_player(player_id)
        return _ok({"deleted": player_id})

    # ── Admin ─────────────────────────────────────────────────────────────

    def admin_login(self, pin: str, ip: str) -> ApiResult:
        if not self.limiter.allow(
            f"admin:{ip}",
            capacity=RATE_ADMIN_LOGIN_PER_MIN,
            refill_per_min=RATE_ADMIN_LOGIN_PER_MIN,
        ):
            return _err(429, "E_RATE", "too many login attempts")
        token = self.admin.verify(pin)
        if token is None:
            return _err(401, "E_PIN", "incorrect PIN")
        return _ok({"admin_token": token})

    def _require_admin(self, admin_token: str | None) -> ApiResult | None:
        """Return an error result if the admin token is invalid, else None."""
        if self.admin.check(admin_token):
            return None
        return _err(401, "E_PIN", "admin session missing or expired")

    def admin_export(self, admin_token: str | None) -> tuple[int, str, str]:
        """Anonymised CSV export. Returns (status, content_type, body).

        Different return shape from the other methods because the response is a
        file download rather than JSON. The adapter handles the distinction.

        Anonymised means: no nickname, no card reference, no IP. Only the
        pseudonymous player_id and the per-answer measurements, which is what the
        analytics actually need. This is the backup for R7 in the architecture —
        ``data/`` is excluded from app exports, so this is how the dataset leaves
        the board.
        """
        denied = self._require_admin(admin_token)
        if denied is not None:
            return denied[0], "application/json", '{"ok": false, "err": "E_PIN"}'

        buf = io.StringIO()
        writer = csv.writer(buf)
        writer.writerow([
            "player_id", "session_token", "round_index", "item_id",
            "label_correct", "label_answered", "correct", "timed_out",
            "limit_ms", "remaining_ms", "reaction_ms", "streak_before",
            "speed", "multiplier", "points", "answer_source", "ack_missing",
            "answered_wall",
        ])
        rows = db().execute(
            """SELECT a.player_id, a.session_token, a.round_index, a.item_id,
                      a.label_correct, a.label_answered, a.correct, a.timed_out,
                      a.limit_ms, a.remaining_ms, a.reaction_ms, a.streak_before,
                      a.speed, a.multiplier, a.points, a.answer_source,
                      a.ack_missing, a.answered_wall
               FROM answers a
               ORDER BY a.answered_mono"""
        ).fetchall()
        for r in rows:
            writer.writerow([r[k] for k in (
                "player_id", "session_token", "round_index", "item_id",
                "label_correct", "label_answered", "correct", "timed_out",
                "limit_ms", "remaining_ms", "reaction_ms", "streak_before",
                "speed", "multiplier", "points", "answer_source", "ack_missing",
                "answered_wall",
            )])
        logger.info("admin exported %d answer rows", len(rows))
        return 200, "text/csv; charset=utf-8", buf.getvalue()

    def admin_delete_player(self, admin_token: str | None, player_id: str) -> ApiResult:
        denied = self._require_admin(admin_token)
        if denied is not None:
            return denied
        if not player_id:
            return _err(400, "E_INTERNAL", "player_id is required")
        self._delete_player(player_id)
        return _ok({"deleted": player_id})

    def admin_db_reset(self, admin_token: str | None, confirm: str) -> ApiResult:
        denied = self._require_admin(admin_token)
        if denied is not None:
            return denied
        if confirm != DB_RESET_CONFIRM:
            # Requiring the literal string rather than a boolean means the caller
            # has to type the word, which survives a mis-clicked confirm dialog.
            return _err(400, "E_INTERNAL", f"confirm must be exactly {DB_RESET_CONFIRM}")

        conn = db()
        with conn:
            conn.execute("DELETE FROM answers")
            conn.execute("DELETE FROM tap_log")
            conn.execute("DELETE FROM sessions")
            conn.execute("DELETE FROM players")
        self.engine.store.clear()
        self.limiter.clear()
        self.join_attempts.clear()
        logger.warning("admin reset the database: all players, sessions and answers deleted")
        return _ok({"reset": True})

    def admin_clock(self, admin_token: str | None, offset_ms: int) -> ApiResult:
        denied = self._require_admin(admin_token)
        if denied is not None:
            return denied
        self.clock.set(int(offset_ms))
        conn = db()
        conn.execute(
            "INSERT INTO meta(key, value) VALUES ('clock_offset_ms', ?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            (str(int(offset_ms)),),
        )
        logger.info("admin set clock offset to %d ms", offset_ms)
        return _ok({"clock_offset_ms": int(offset_ms)})

    # ── Shared internals ──────────────────────────────────────────────────

    def _delete_player(self, player_id: str) -> None:
        """Hard-delete a player's activity and tombstone the player row.

        The row is tombstoned with a randomised nickname rather than deleted, so
        the leaderboard's historical scores stay honest. Deleting the row outright
        would either orphan the sessions that reference it or, with the foreign
        key enforced, fail; and silently dropping a score from the leaderboard
        after the fact would misrepresent everyone else's ranking.
        """
        conn = db()
        with conn:
            conn.execute("DELETE FROM answers WHERE player_id = ?", (player_id,))
            conn.execute("DELETE FROM tap_log WHERE player_id = ?", (player_id,))
            conn.execute("DELETE FROM sessions WHERE player_id = ?", (player_id,))
            conn.execute(
                "UPDATE players SET deleted = 1, nickname = ?, consent_version = NULL "
                "WHERE player_id = ?",
                (f"[deleted-{secrets.token_hex(4)}]", player_id),
            )
        logger.info("deleted all data for player %s", player_id)
