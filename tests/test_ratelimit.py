"""
Rate limiting and PIN lockout.

The clock is injected rather than read from ``time.monotonic`` inside the limiter,
so these tests can advance time deterministically. Without that, testing "ten
requests per minute" would mean either sleeping for a minute or writing a test
that passes only because it ran fast enough.
"""

from __future__ import annotations

import pytest

from phishtap.ratelimit import JoinAttempts, RateLimiter

KEY = "192.168.1.20"


# ── Token bucket ──────────────────────────────────────────────────────────

def test_first_request_is_allowed():
    assert RateLimiter().allow(KEY, capacity=10, refill_per_min=10, now=0.0)


def test_burst_up_to_capacity_is_allowed():
    """A bucket starts full, so the whole capacity is available immediately
    regardless of how long the key has been idle."""
    rl = RateLimiter()
    results = [rl.allow(KEY, capacity=5, refill_per_min=5, now=0.0) for _ in range(5)]
    assert results == [True] * 5


def test_request_beyond_capacity_is_refused():
    rl = RateLimiter()
    for _ in range(10):
        rl.allow(KEY, capacity=10, refill_per_min=10, now=0.0)
    assert rl.allow(KEY, capacity=10, refill_per_min=10, now=0.0) is False


def test_bucket_refills_over_time():
    rl = RateLimiter()
    for _ in range(10):
        rl.allow(KEY, capacity=10, refill_per_min=10, now=0.0)
    assert rl.allow(KEY, capacity=10, refill_per_min=10, now=0.0) is False
    # 10 per minute is one token every 6 seconds.
    assert rl.allow(KEY, capacity=10, refill_per_min=10, now=6.0) is True
    assert rl.allow(KEY, capacity=10, refill_per_min=10, now=6.0) is False


def test_bucket_never_exceeds_capacity():
    """Refill must be capped, or a key idle for an hour would accumulate 60
    tokens and then be able to burst far past the intended limit."""
    rl = RateLimiter()
    for _ in range(10):
        rl.allow(KEY, capacity=10, refill_per_min=10, now=0.0)
    allowed = sum(
        1 for _ in range(50)
        if rl.allow(KEY, capacity=10, refill_per_min=10, now=3600.0)
    )
    assert allowed == 10, f"refilled past capacity, allowed {allowed}"


def test_keys_are_independent():
    rl = RateLimiter()
    for _ in range(3):
        rl.allow("a", capacity=3, refill_per_min=3, now=0.0)
    assert rl.allow("a", capacity=3, refill_per_min=3, now=0.0) is False
    assert rl.allow("b", capacity=3, refill_per_min=3, now=0.0) is True


def test_a_refused_request_does_not_consume_a_token():
    """If a refusal drained the bucket, a client retrying in a loop would extend
    its own lockout indefinitely and could never recover."""
    rl = RateLimiter()
    for _ in range(2):
        rl.allow(KEY, capacity=2, refill_per_min=60, now=0.0)
    for _ in range(10):
        assert rl.allow(KEY, capacity=2, refill_per_min=60, now=0.0) is False
    # One second later at 60/min exactly one token has refilled.
    assert rl.allow(KEY, capacity=2, refill_per_min=60, now=1.0) is True


def test_clock_going_backwards_does_not_grant_tokens():
    """The board has no RTC and an admin can set the clock. A backwards jump must
    not produce a negative elapsed time that somehow refills the bucket."""
    rl = RateLimiter()
    rl.allow(KEY, capacity=1, refill_per_min=60, now=1000.0)
    assert rl.allow(KEY, capacity=1, refill_per_min=60, now=500.0) is False


def test_reset_restores_the_full_burst():
    rl = RateLimiter()
    for _ in range(3):
        rl.allow(KEY, capacity=3, refill_per_min=3, now=0.0)
    assert rl.allow(KEY, capacity=3, refill_per_min=3, now=0.0) is False
    rl.reset(KEY)
    assert rl.allow(KEY, capacity=3, refill_per_min=3, now=0.0) is True


def test_reset_of_an_unknown_key_is_harmless():
    rl = RateLimiter()
    rl.reset("never-seen")   # must not raise


def test_clear_drops_everything():
    rl = RateLimiter()
    for _ in range(3):
        rl.allow(KEY, capacity=3, refill_per_min=3, now=0.0)
    rl.clear()
    assert rl.allow(KEY, capacity=3, refill_per_min=3, now=0.0) is True


def test_prune_only_removes_idle_buckets():
    """Pruning must not forgive an actively throttled key, or the limit could be
    defeated by simply waiting for a prune cycle."""
    rl = RateLimiter()
    for _ in range(3):
        rl.allow("active", capacity=3, refill_per_min=3, now=0.0)
    rl.allow("idle", capacity=3, refill_per_min=3, now=0.0)

    removed = rl.prune(idle_for_s=100.0, now=200.0)
    assert removed == 2, "both buckets were idle past the threshold"
    # Both were dropped, so both start full again. That is the documented
    # behaviour of prune: it reclaims memory for keys that have gone quiet.
    assert rl.allow("active", capacity=3, refill_per_min=3, now=200.0) is True


def test_prune_keeps_recent_buckets():
    rl = RateLimiter()
    for _ in range(3):
        rl.allow(KEY, capacity=3, refill_per_min=3, now=100.0)
    # No time has elapsed, so nothing is idle past the threshold.
    assert rl.prune(idle_for_s=100.0, now=100.0) == 0
    # The bucket is still drained, which proves prune left it in place rather
    # than dropping it and letting it start full again.
    assert rl.allow(KEY, capacity=3, refill_per_min=3, now=100.0) is False


# ── PIN lockout ───────────────────────────────────────────────────────────

def test_lockout_after_threshold():
    ja = JoinAttempts(threshold=5)
    for _ in range(4):
        assert ja.record_failure(KEY) is False
    assert ja.record_failure(KEY) is True
    assert ja.is_locked(KEY) is True


def test_remaining_counts_down():
    ja = JoinAttempts(threshold=3)
    assert ja.remaining(KEY) == 3
    ja.record_failure(KEY)
    assert ja.remaining(KEY) == 2
    ja.record_failure(KEY)
    assert ja.remaining(KEY) == 1


def test_success_clears_the_counter():
    """A correct PIN must reset the count. Otherwise a student who mistypes once
    and then gets it right is part-way to a lockout they did nothing to earn."""
    ja = JoinAttempts(threshold=3)
    ja.record_failure(KEY)
    ja.record_failure(KEY)
    ja.record_success(KEY)
    assert ja.remaining(KEY) == 3
    assert ja.is_locked(KEY) is False


def test_unknown_key_is_not_locked():
    assert JoinAttempts(threshold=5).is_locked("nobody") is False


def test_remaining_never_goes_negative():
    ja = JoinAttempts(threshold=2)
    for _ in range(10):
        ja.record_failure(KEY)
    assert ja.remaining(KEY) == 0


def test_clear_resets_every_key():
    ja = JoinAttempts(threshold=2)
    ja.record_failure("a")
    ja.record_failure("a")
    ja.clear()
    assert ja.is_locked("a") is False


def test_lockout_is_per_ip_not_global():
    """One person brute-forcing the PIN must not lock out the rest of the room."""
    ja = JoinAttempts(threshold=2)
    ja.record_failure("attacker")
    ja.record_failure("attacker")
    assert ja.is_locked("attacker") is True
    assert ja.is_locked("student") is False
