"""
PhishTap Lite — mock bridge, for laptop development and tests.

Simulates the MCU side of the protocol in-process, with no board, no Docker and
no ``arduino.*`` import. The engine, the API and the pytest suite all run against
this, which is what makes the whole backend developable on a laptop.

FAITHFUL WHERE IT MATTERS
The mock is not a stub that returns canned values. It models the parts of the MCU
that the backend's correctness actually depends on:

  * the event ring, including monotonic sequence numbers and the ``since`` cursor
  * the state machine's refusal to accept an unknown state name
  * the round limit bounds, so an out-of-range ``start_round`` is rejected the
    same way the firmware rejects it
  * the lives clamp at three

If the mock were more permissive than the firmware, a test could pass here and
fail on the board, which is worse than having no test at all.

Test hooks (``inject_tap``, ``inject_timeout``) let a test drive the backend the
way a student tapping a card would, without any hardware.
"""

from __future__ import annotations

import threading
from typing import Any

from ..bridge_port import BridgeError
from ..clock import monotonic_ms
from ..config import (
    PROTO_VERSION,
    FW_VERSION,
    ROUND_LIMIT_MIN_S,
    ROUND_LIMIT_MAX_S,
    STARTING_LIVES,
)

VALID_STATES = frozenset(
    {"idle", "tapped", "playing", "over", "busy", "error"}
)
VALID_BUZZ = frozenset(
    {"welcome", "join", "ok", "wrong", "tick", "busy", "over", "error"}
)
VALID_RESULTS = frozenset({"correct", "wrong", "timeout"})


class MockBridge:
    """An in-process stand-in for the MCU.

    Satisfies ``BridgePort``. Thread-safe, because the API layer may call it from
    several Socket.IO callback threads at once, exactly as it would call the real
    bridge.
    """

    def __init__(self, *, rfid_version: int = 0x82) -> None:
        self._lock = threading.RLock()

        self._state = "idle"
        self._lives = STARTING_LIVES
        self._round = 0
        self._score = 0
        self._rfid_ok = True
        self._rfid_version = rfid_version
        self._rfid_state = "seek"

        # Event ring, mirroring the firmware's 16-slot design.
        self._events: list[dict[str, Any]] = []
        self._next_seq = 1
        self._oldest_seq = 1
        self._overflows = 0
        self._ring_capacity = 16

        # Recorded calls, so a test can assert on what the backend asked the MCU
        # to do. This is the mock's main advantage over real hardware: the
        # firmware's side effects are observable.
        self.calls: list[tuple[str, tuple[Any, ...]]] = []

        self._started_mono = monotonic_ms()
        self._last_ack_seq = 0

    # ── Test hooks ────────────────────────────────────────────────────────

    def inject_tap(self, uid_hex: str = "C7DA4104", uid_len: int = 4) -> int:
        """Queue a card_tap event as if the reader had seen a card.

        Returns the sequence number assigned, so a test can assert on ordering.
        """
        return self._push(
            "card_tap",
            {"uid_hex": uid_hex, "uid_len": uid_len, "t_ms": monotonic_ms()},
        )

    def inject_removal(self) -> int:
        return self._push("card_removed", {})

    def inject_timeout(self, round_index: int | None = None) -> int:
        return self._push(
            "round_timeout", {"round": self._round if round_index is None else round_index}
        )

    def inject_fault(self, code: str = "E_RFID_READ") -> int:
        return self._push("hw_fault", {"code": code})

    def set_rfid_ok(self, ok: bool, version: int | None = None) -> None:
        """Simulate the reader failing or recovering."""
        with self._lock:
            self._rfid_ok = ok
            if version is not None:
                self._rfid_version = version

    def reset(self) -> None:
        """Return to the post-boot state. Used between tests."""
        with self._lock:
            self.__init__()  # type: ignore[misc]

    # ── Internals ─────────────────────────────────────────────────────────

    def _push(self, event_type: str, data: dict[str, Any]) -> int:
        with self._lock:
            seq = self._next_seq
            self._next_seq += 1
            self._events.append(
                {
                    "seq": seq,
                    "t_ms": monotonic_ms(),
                    "type": event_type,
                    "data": data,
                }
            )
            # Mirror the firmware's eviction: the ring is bounded and the oldest
            # event is dropped when it is full.
            if len(self._events) > self._ring_capacity:
                dropped = len(self._events) - self._ring_capacity
                self._events = self._events[dropped:]
                self._oldest_seq += dropped
                self._overflows += dropped
            return seq

    def _record(self, name: str, *args: Any) -> None:
        with self._lock:
            self.calls.append((name, args))

    def calls_of(self, name: str) -> list[tuple[Any, ...]]:
        """Every recorded call to ``name``, in order."""
        with self._lock:
            return [a for n, a in self.calls if n == name]

    # ── BridgePort: reads ─────────────────────────────────────────────────

    def get_status(self) -> dict[str, Any]:
        with self._lock:
            return {
                "proto": PROTO_VERSION,
                "fw": FW_VERSION,
                "state": self._state,
                "uptime_ms": monotonic_ms() - self._started_mono,
                "rfid_ok": self._rfid_ok,
                "matrix_ok": True,
                "lives": self._lives,
                "round": self._round,
                "score": self._score,
                "queue_len": len(self._events),
                "overflows": self._overflows,
                "link_ok": True,
                "rfid_version": self._rfid_version,
                "rfid_version_documented": self._rfid_version in (0x88, 0x90, 0x91, 0x92),
                "rfid_state": self._rfid_state,
            }

    def get_events(self, since: int) -> tuple[list[dict[str, Any]], int, bool]:
        with self._lock:
            start = since + 1
            missed = False
            if start < self._oldest_seq:
                if since != 0:
                    missed = True
                start = self._oldest_seq
            out = [dict(e) for e in self._events if e["seq"] >= start]
            oldest = self._oldest_seq if self._events else 0
            return out, oldest, missed

    def selftest(self) -> dict[str, Any]:
        with self._lock:
            return {
                "leds": True,
                "buzzer": True,
                "matrix": True,
                "rgb": True,
                "rfid": self._rfid_ok,
                "details": {
                    "matrix_has_readback": False,
                    "boot_selftest_done": True,
                    "rfid_version": self._rfid_version,
                    "rfid_version_documented": self._rfid_version in (0x88, 0x90, 0x91, 0x92),
                    "rfid_state": self._rfid_state,
                    "link_ok": True,
                    "ring_overflows": self._overflows,
                    "ring_pushed": self._next_seq - 1,
                },
            }

    # ── BridgePort: writes ────────────────────────────────────────────────

    def ack_events(self, seq: int) -> None:
        self._record("ack_events", seq)
        with self._lock:
            self._last_ack_seq = max(self._last_ack_seq, seq)
            self._events = [e for e in self._events if e["seq"] > seq]
            if self._events:
                self._oldest_seq = self._events[0]["seq"]
            else:
                self._oldest_seq = seq + 1

    def set_state(self, state: str) -> None:
        self._record("set_state", state)
        # The firmware rejects an unknown state name rather than defaulting, and
        # so does the mock. A test that passes here must pass on the board.
        if state not in VALID_STATES:
            raise BridgeError(f"mock MCU rejected unknown state {state!r}")
        with self._lock:
            self._state = state

    def start_round(self, round_index: int, limit_ms: int) -> None:
        self._record("start_round", round_index, limit_ms)
        lo = int(ROUND_LIMIT_MIN_S * 1000)
        hi = int(ROUND_LIMIT_MAX_S * 1000)
        if not (lo <= limit_ms <= hi):
            raise BridgeError(
                f"mock MCU rejected limit_ms={limit_ms}, outside [{lo}, {hi}]"
            )
        with self._lock:
            self._round = round_index
            self._state = "playing"

    def end_round(self, result: str) -> None:
        self._record("end_round", result)
        if result not in VALID_RESULTS:
            raise BridgeError(f"mock MCU rejected unknown result {result!r}")
        with self._lock:
            if result in ("wrong", "timeout"):
                self._lives = max(0, self._lives - 1)

    def set_lives(self, n: int) -> None:
        self._record("set_lives", n)
        if not (0 <= n <= STARTING_LIVES):
            raise BridgeError(f"mock MCU rejected lives={n}")
        with self._lock:
            self._lives = n

    def show_score(self, score: int) -> None:
        self._record("show_score", score)
        with self._lock:
            self._score = score

    def buzz(self, pattern: str) -> None:
        self._record("buzz", pattern)
        if pattern not in VALID_BUZZ:
            raise BridgeError(f"mock MCU rejected unknown buzz pattern {pattern!r}")
