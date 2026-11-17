"""
PhishTap Lite — privacy layer.

Pseudonymises card UIDs so the raw UID never reaches disk or a log line.

``player_id = HMAC-SHA256(server_secret, identity_bytes).hexdigest()[:16]``

This is pseudonymisation, not anonymisation: the same card (plus nickname, in
shared-card mode) always maps to the same player_id, so history and consent
persist. The consent screen says so plainly.

The raw UID exists only in the bridge event payload, the HMAC input, and then
nowhere. A logging filter redacts any ``uid_hex`` key so a stray log line
cannot leak it.

``server_secret`` is 32 random bytes generated once at first boot, stored
mode 0600, never in the database, never logged. Losing it makes every
existing player_id irrecoverable, which is the correct failure mode for a
secret that exists to protect identity.
"""

from __future__ import annotations

import hashlib
import hmac
import logging
import os
import unicodedata
from pathlib import Path
from typing import Any

from .config import SERVER_SECRET_PATH, IDENTITY_MODE, NICKNAME_MIN_LEN, NICKNAME_MAX_LEN


# ── Server secret ─────────────────────────────────────────────────────────

def _load_or_create_secret(path: Path = SERVER_SECRET_PATH) -> bytes:
    """Load the server secret, creating it on first boot.

    32 random bytes from os.urandom, which on CPython reads from /dev/urandom
    on Linux. Stored mode 0600 so only the arduino user (or the laptop user)
    can read it. Never written to the database, never logged.
    """
    from .config import ensure_data_dir
    ensure_data_dir()

    if path.exists():
        return path.read_bytes()

    secret = os.urandom(32)
    # Write with restrictive permissions. os.umask is process-wide, so set the
    # mode explicitly after writing rather than relying on the umask.
    path.write_bytes(secret)
    try:
        path.chmod(0o600)
    except OSError:
        # On some filesystems chmod is a no-op or unsupported. The secret is
        # still secret to other users on the same machine only if the file is
        # not world-readable, but on the board the app runs as the arduino user
        # and the data dir is not shared. Best-effort.
        pass
    return secret


_SERVER_SECRET: bytes | None = None


def server_secret() -> bytes:
    """Return the process-wide server secret, loading it on first use."""
    global _SERVER_SECRET
    if _SERVER_SECRET is None:
        _SERVER_SECRET = _load_or_create_secret()
    return _SERVER_SECRET


# ── Identity ──────────────────────────────────────────────────────────────

def _normalise_nickname(raw: str) -> str:
    """NFKC-normalise, trim, and case-fold a nickname for identity purposes.

    NFKC folds visually-identical characters (e.g. fullwidth 'Ａ' -> 'A') so two
    nicknames that look the same to a human produce the same identity bytes.
    Case-folding is preferred over lowercasing because it handles non-ASCII
    correctly (e.g. 'İ' -> 'i̇').
    """
    return unicodedata.normalize("NFKC", raw).strip().casefold()


def identity_bytes(uid_hex: str, nickname: str | None) -> bytes:
    """Compute the identity bytes for the HMAC input.

    In ``shared_card`` mode (demo default), the nickname is folded in so that
    several people sharing one card stay distinct. A 0x00 separator prevents
    ``uid || nickname`` collisions across different UID/nickname boundaries
    (e.g. uid="AB" + nick="CD" vs uid="ABC" + nick="D").

    In ``per_card`` mode, the UID alone is the identity, and the nickname is
    only a display label.
    """
    uid = uid_hex.encode("ascii", errors="ignore")
    if IDENTITY_MODE == "per_card":
        return uid

    if nickname is None or nickname == "":
        # Without a nickname in shared-card mode, every player on the same card
        # collapses into one. That is a known limitation of the demo mode, not
        # a crash. Return the UID alone so at least the card is identified.
        return uid

    nick = _normalise_nickname(nickname).encode("utf-8", errors="ignore")
    return uid + b"\x00" + nick


def player_id(uid_hex: str, nickname: str | None = None) -> str:
    """Compute the 16-hex-character player_id.

    Truncated to 16 characters (64 bits) rather than the full 64, because a
    player_id collision at 1 in 18 quintillion is not a realistic concern for a
    college demo with a few hundred players, and a shorter ID is easier to
    display in an admin panel.
    """
    mac = hmac.new(server_secret(), identity_bytes(uid_hex, nickname), hashlib.sha256)
    return mac.hexdigest()[:16]


# ── Nickname validation ───────────────────────────────────────────────────

# A small, conservative profanity filter. Not exhaustive, and not meant to be:
# the goal is to catch the obvious cases without maintaining a word list. The
# admin can delete a player through the admin panel if something slips through.
_BLOCKED_NICKNAME_FRAGMENTS: frozenset[str] = frozenset(
    {
        # English
        "fuck", "shit", "bitch", "asshole", "dick", "cunt", "bastard",
        # Hindi transliterations that would make the demo awkward
        "bhosdi", "madarchod", "behenchod", "chutiya", "gaand", "lund",
        "lauda", "randi",
    }
)


def validate_nickname(raw: str) -> tuple[bool, str]:
    """Validate a nickname. Returns (ok, normalised_or_error).

    On success, the second element is the normalised nickname for DISPLAY
    (NFKC + trim, NOT case-folded, so the player sees what they typed).
    On failure, it is a human-readable reason.
    """
    if raw is None:
        return False, "nickname is required"

    # NFKC-normalise and trim for length checking. Do NOT case-fold here: the
    # display name preserves case, only the identity bytes are case-folded.
    display = unicodedata.normalize("NFKC", raw).strip()

    if len(display) < NICKNAME_MIN_LEN:
        return False, f"nickname must be at least {NICKNAME_MIN_LEN} characters"
    if len(display) > NICKNAME_MAX_LEN:
        return False, f"nickname must be at most {NICKNAME_MAX_LEN} characters"

    folded = display.casefold()
    for fragment in _BLOCKED_NICKNAME_FRAGMENTS:
        if fragment in folded:
            return False, "nickname contains a blocked word"

    return True, display


# ── Logging filter ────────────────────────────────────────────────────────

class UidRedactionFilter(logging.Filter):
    """Redact any ``uid_hex`` key in a log record's extra dict.

    Attached to the root logger at startup. A UID is privacy-sensitive: it
    identifies a physical card, and in shared-card mode it is part of the
    player identity. Letting it reach a log file would defeat the
    pseudonymisation the HMAC provides.

    The filter replaces the value with ``[REDACTED]`` rather than dropping the
    record, because the fact that a UID was processed is useful for debugging
    while the value itself is not.
    """

    def filter(self, record: logging.LogRecord) -> bool:
        for key in ("uid_hex", "uid", "card_uid"):
            if hasattr(record, key):
                setattr(record, key, "[REDACTED]")
        return True


def install_uid_redaction() -> None:
    """Attach the redaction filter to the root logger."""
    root = logging.getLogger()
    if not any(isinstance(f, UidRedactionFilter) for f in root.filters):
        root.addFilter(UidRedactionFilter())


def redact_for_log(obj: dict[str, Any]) -> dict[str, Any]:
    """Return a copy of ``obj`` with any uid-like key redacted.

    Used when logging a dict directly (e.g. via ``logger.info("event %s", d)``)
    rather than through extra=, because the filter only sees LogRecord
    attributes, not arbitrary formatted arguments.
    """
    out = dict(obj)
    for key in ("uid_hex", "uid", "card_uid"):
        if key in out:
            out[key] = "[REDACTED]"
    return out
