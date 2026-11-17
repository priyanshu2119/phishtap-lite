"""
PhishTap Lite — rate limiting.

In-memory token buckets, keyed per IP and per session token, on a monotonic
clock.

They survive nothing, and that is deliberate. A restart clearing the buckets is
the correct failure mode: a restart also clears the in-flight sessions the
buckets were protecting, so there is nothing left to abuse. Persisting rate
limits would mean a student who triggered a limit before a crash is still
locked out after it, for no security benefit.

Monotonic rather than wall-clock, because the board has no RTC and the clock can
jump. A wall-clock bucket would either never refill or refill instantly the
moment an admin corrected the time.
"""

from __future__ import annotations

import threading
from dataclasses import dataclass

from .clock import monotonic_s


@dataclass
class _Bucket:
    tokens: float
    last_refill: float


class RateLimiter:
    """A registry of named token buckets.

    Thread-safe: Socket.IO callbacks run on their own threads and several can
    hit the same key concurrently. A single lock around the whole registry is
    coarse but correct, and at the request rates involved (a classroom, not the
    internet) contention is not a concern.
    """

    def __init__(self) -> None:
        self._buckets: dict[str, _Bucket] = {}
        self._lock = threading.Lock()

    def allow(
        self,
        key: str,
        *,
        capacity: int,
        refill_per_min: float,
        now: float | None = None,
    ) -> bool:
        """Consume one token from ``key``'s bucket. True if allowed.

        ``capacity`` is the burst size and ``refill_per_min`` the sustained rate.
        A bucket starts full, so the first ``capacity`` requests in a burst are
        always allowed regardless of how long the key has been idle.
        """
        ts = monotonic_s() if now is None else now
        refill_per_sec = refill_per_min / 60.0

        with self._lock:
            bucket = self._buckets.get(key)
            if bucket is None:
                # First sight of this key: start full, minus the one we are
                # about to consume.
                self._buckets[key] = _Bucket(tokens=float(capacity - 1), last_refill=ts)
                return True

            elapsed = max(0.0, ts - bucket.last_refill)
            bucket.tokens = min(float(capacity), bucket.tokens + elapsed * refill_per_sec)
            bucket.last_refill = ts

            if bucket.tokens >= 1.0:
                bucket.tokens -= 1.0
                return True
            return False

    def reset(self, key: str) -> None:
        """Drop a key's bucket, restoring its full burst allowance."""
        with self._lock:
            self._buckets.pop(key, None)

    def clear(self) -> None:
        """Drop every bucket. Used by tests and by an admin reset."""
        with self._lock:
            self._buckets.clear()

    def prune(self, *, idle_for_s: float = 3600.0, now: float | None = None) -> int:
        """Remove buckets that have been idle and are full again.

        Without this the registry grows for the lifetime of the process, one
        entry per distinct IP and token ever seen. A classroom demo will not
        exhaust memory, but a console left running for a week at a college fest
        might, and an unbounded cache is a leak whether or not anyone notices.

        Only full buckets are pruned: dropping a partially-drained one would
        silently forgive a limiter that was actively throttling someone.
        Returns the number removed.
        """
        ts = monotonic_s() if now is None else now
        removed = 0
        with self._lock:
            for key in list(self._buckets.keys()):
                b = self._buckets[key]
                if ts - b.last_refill >= idle_for_s:
                    del self._buckets[key]
                    removed += 1
        return removed


class JoinAttempts:
    """Tracks wrong PIN attempts per IP, for the lockout in /api/join.

    Separate from the token bucket because the semantics differ: a bucket limits
    RATE, while this counts consecutive FAILURES and locks out after a threshold.
    A correct PIN clears the counter, so someone who mistypes once and then gets
    it right is not part-way to a lockout.
    """

    def __init__(self, threshold: int) -> None:
        self._threshold = threshold
        self._fails: dict[str, int] = {}
        self._lock = threading.Lock()

    def record_failure(self, key: str) -> bool:
        """Record a wrong PIN. Returns True if the key is now locked out."""
        with self._lock:
            n = self._fails.get(key, 0) + 1
            self._fails[key] = n
            return n >= self._threshold

    def record_success(self, key: str) -> None:
        with self._lock:
            self._fails.pop(key, None)

    def is_locked(self, key: str) -> bool:
        with self._lock:
            return self._fails.get(key, 0) >= self._threshold

    def remaining(self, key: str) -> int:
        with self._lock:
            return max(0, self._threshold - self._fails.get(key, 0))

    def clear(self) -> None:
        with self._lock:
            self._fails.clear()
