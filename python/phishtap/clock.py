"""
PhishTap Lite — monotonic clock and wall-clock offset.

The board has no RTC and may be offline, so wall-clock time can jump. Every
duration the system measures — reaction time, round length, session age — is
a monotonic delta, which is immune to clock jumps. Wall-clock is stored only
for human-readable timestamps in the database and the admin export.

``meta.clock_offset_ms`` is admin-settable and ``sessions.clock_offset_ms``
snapshots the offset in force when the session started, so a later admin
correction cannot retroactively change what a past session recorded.
"""

from __future__ import annotations

import time
from typing import Final

# A monotonic clock that does not go backwards. time.monotonic() is the right
# choice on both CPython 3.13 (board) and the laptop dev environment.
_MONOTONIC: Final = time.monotonic


def monotonic_ms() -> int:
    """Monotonic time in milliseconds, as an int.

    Used for every duration measurement. Returns an int rather than a float so
    that subtraction and comparison never accumulate floating-point error across
    a long session.
    """
    return int(_MONOTONIC() * 1000)


def monotonic_s() -> float:
    """Monotonic time in seconds, as a float."""
    return _MONOTONIC()


def wall_now_iso() -> str:
    """Wall-clock now as an ISO 8601 string, for human-readable timestamps only.

    Never used for a duration. If the clock is wrong (no RTC, offline), the
    timestamp is wrong, but every duration computed from monotonic_ms() is
    still correct.
    """
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


class ClockOffset:
    """Tracks the offset between wall-clock and the real world.

    The board's wall clock may be wrong by hours or days if it has no RTC and
    no NTP. An admin who knows the real time can set the offset, and every
    timestamp written afterwards is corrected by it.

    The offset is held in memory and persisted in the meta table. It is a
    signed integer in milliseconds: ``corrected = wall_ms + offset_ms``.
    """

    def __init__(self, initial_ms: int = 0) -> None:
        self._offset_ms: int = initial_ms

    @property
    def offset_ms(self) -> int:
        return self._offset_ms

    def set(self, offset_ms: int) -> None:
        self._offset_ms = offset_ms

    def corrected_iso(self) -> str:
        """Wall-clock now, corrected by the offset, as ISO 8601."""
        corrected = time.time() + (self._offset_ms / 1000.0)
        return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(corrected))
