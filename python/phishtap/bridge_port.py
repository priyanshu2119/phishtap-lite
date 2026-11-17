"""
PhishTap Lite — the bridge port interface.

Defines what the game engine needs from the MCU, without saying how it is
provided. Two implementations exist:

  * ``adapters/bridge_adapter.py`` — the real one, over ``arduino.app_utils.Bridge``
  * ``adapters/mock_bridge.py``     — an in-process stand-in for laptop development

WHY THIS IS A SEPARATE MODULE
The obvious place for the interface is ``bridge_adapter.py``, but that module
imports ``arduino.app_utils`` at module level, which does not exist off the
board. If ``mock_bridge`` imported the interface from there, laptop development
and the whole pytest suite would fail at import time — defeating the entire
purpose of having a mock.

So the contract lives here, in pure Python with no App Lab dependency, and both
adapters import it. Neither adapter imports the other.

This is a ``typing.Protocol`` with ``runtime_checkable``, so a test can assert
that an object actually satisfies the contract rather than trusting that it does.
"""

from __future__ import annotations

from typing import Any, Protocol, runtime_checkable


@runtime_checkable
class BridgePort(Protocol):
    """The MCU-facing surface the engine depends on.

    Method names mirror the RPC methods in docs/ARCHITECTURE.md §4 with the
    ``pt.`` prefix dropped, since the prefix is a wire detail the engine should
    not have to repeat.

    Every method that talks to the MCU can fail. Implementations raise
    ``BridgeError`` for a transport-level failure (timeout, connection lost,
    method not registered) and return normally with a parsed payload otherwise.
    Callers are expected to treat ``BridgeError`` as "the hardware is
    unreachable right now" and degrade, not crash.
    """

    # ── Reads ─────────────────────────────────────────────────────────────

    def get_status(self) -> dict[str, Any]:
        """Return the MCU's status payload.

        Keys: proto, fw, state, uptime_ms, rfid_ok, matrix_ok, lives, round,
        score, queue_len, overflows, link_ok, rfid_version,
        rfid_version_documented, rfid_state.
        """
        ...

    def get_events(self, since: int) -> tuple[list[dict[str, Any]], int, bool]:
        """Drain events with ``seq > since``.

        Returns ``(events, oldest_seq, overflow)``. ``overflow`` is True when the
        cursor fell behind the ring and some events were dropped before they could
        be read, which means the caller should resync and log rather than assume
        it saw everything.
        """
        ...

    def selftest(self) -> dict[str, Any]:
        """Return the MCU's self-test report."""
        ...

    # ── Writes ────────────────────────────────────────────────────────────

    def ack_events(self, seq: int) -> None:
        """Tell the MCU that everything up to ``seq`` has been consumed."""
        ...

    def set_state(self, state: str) -> None:
        """Command the MCU into a state: idle, tapped, playing, over, busy, error."""
        ...

    def start_round(self, round_index: int, limit_ms: int) -> None:
        """Arm the MCU's countdown for a round."""
        ...

    def end_round(self, result: str) -> None:
        """Report a round outcome: ``correct``, ``wrong`` or ``timeout``."""
        ...

    def set_lives(self, n: int) -> None:
        """Set the lives LED count."""
        ...

    def show_score(self, score: int) -> None:
        """Show the final score on the matrix."""
        ...

    def buzz(self, pattern: str) -> None:
        """Play a buzzer rhythm: welcome, join, ok, wrong, tick, busy, over, error."""
        ...


class BridgeError(RuntimeError):
    """The MCU could not be reached, or rejected the call.

    A single exception type rather than one per failure mode, because the caller
    almost always wants the same response to all of them: mark the link down,
    pause the game, tell the user. Distinguishing a timeout from an unregistered
    method matters for debugging, so the original cause is chained and the
    message says which it was.
    """


class LinkDown(BridgeError):
    """The MCU is unreachable at all, as opposed to rejecting one call.

    Separated from ``BridgeError`` because the recovery differs: a rejected call
    is usually a bug worth logging and continuing past, while a down link means
    the kiosk should show a hardware-fault banner and the game should pause.
    """
