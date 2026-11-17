"""
PhishTap Lite — SQLite database layer.

WAL mode so the Socket.IO callback threads can read concurrently with the
writer. The database lives in the app's ``data/`` folder, which persists
across container restarts on the board (a bind mount) and is a local folder
on the laptop.

Six tables: ``meta``, ``players``, ``sessions``, ``answers``, ``items``,
``tap_log``. Every row stores both a wall-clock timestamp (human-readable,
possibly wrong if no RTC) and a monotonic timestamp (immune to clock jumps,
used for every duration).

Schema is created on first boot and is idempotent. Migrations are not needed
yet; when they are, a ``meta.schema_version`` row will drive them.
"""

from __future__ import annotations

import sqlite3
import threading
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator

from .config import DB_PATH
from .clock import monotonic_ms, wall_now_iso


# A process-wide lock around schema creation. SQLite handles concurrent
# connections fine in WAL mode, but two threads both running CREATE TABLE on
# the first boot would race. After the schema exists this lock is never
# contended.
_schema_lock = threading.Lock()
_schema_initialised = False


def _connect(path: Path = DB_PATH) -> sqlite3.Connection:
    """Open a connection with the settings this project needs.

    ``check_same_thread=False`` because Socket.IO callback threads share the
    connection. WAL mode makes concurrent reads safe; writes are serialised by
    SQLite itself. ``isolation_level=None`` puts the connection in autocommit
    mode, so each statement is its own transaction and there is no need to
    call commit() explicitly — which is the source of more bugs than it saves.
    """
    from .config import ensure_data_dir
    ensure_data_dir()

    conn = sqlite3.connect(str(path), check_same_thread=False, isolation_level=None)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    conn.execute("PRAGMA synchronous=NORMAL")
    return conn


_DB: sqlite3.Connection | None = None
_db_lock = threading.Lock()


def db() -> sqlite3.Connection:
    """Return the process-wide database connection, opening it on first use."""
    global _DB
    if _DB is not None:
        return _DB
    with _db_lock:
        if _DB is None:
            _DB = _connect()
            _init_schema(_DB)
        return _DB


SCHEMA_SQL = """
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS players (
    player_id      TEXT PRIMARY KEY,
    nickname       TEXT NOT NULL,
    consent_version INTEGER,
    consent_at_mono INTEGER,
    consent_at_wall TEXT,
    created_mono   INTEGER NOT NULL,
    created_wall   TEXT NOT NULL,
    deleted        INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS sessions (
    token           TEXT PRIMARY KEY,
    pin             TEXT NOT NULL,
    -- Nullable: a session is created at tap time, before the player has supplied
    -- a nickname and therefore before player_id can be computed in shared-card
    -- mode. It is backfilled when consent is submitted.
    player_id       TEXT,
    -- A keyed HMAC of the card UID, NOT the UID itself. Storing the raw UID here
    -- would violate docs/ARCHITECTURE.md §8 and turn the database into a card
    -- tracker sitting right next to the pseudonym it was meant to protect.
    card_ref        TEXT NOT NULL,
    state           TEXT NOT NULL,
    round_index     INTEGER NOT NULL DEFAULT 0,
    lives           INTEGER NOT NULL DEFAULT 3,
    score           INTEGER NOT NULL DEFAULT 0,
    streak          INTEGER NOT NULL DEFAULT 0,
    created_mono    INTEGER NOT NULL,
    created_wall    TEXT NOT NULL,
    clock_offset_ms INTEGER NOT NULL DEFAULT 0,
    expires_at_mono INTEGER NOT NULL,
    game_over_mono  INTEGER,
    review_token    TEXT,
    review_expires_at_mono INTEGER,
    FOREIGN KEY (player_id) REFERENCES players(player_id)
);

CREATE TABLE IF NOT EXISTS answers (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    session_token   TEXT NOT NULL,
    player_id       TEXT,
    round_index     INTEGER NOT NULL,
    item_id         TEXT NOT NULL,
    label_correct   INTEGER NOT NULL,   -- 0 safe, 1 phish
    label_answered  INTEGER NOT NULL,   -- 0 safe, 1 phish, -1 timed out
    correct         INTEGER NOT NULL,   -- 0 wrong, 1 right
    timed_out       INTEGER NOT NULL DEFAULT 0,
    limit_ms        INTEGER NOT NULL,
    remaining_ms    INTEGER NOT NULL,
    reaction_ms     INTEGER,
    streak_before   INTEGER NOT NULL,
    speed           INTEGER NOT NULL,
    multiplier      REAL NOT NULL,
    points          INTEGER NOT NULL,
    answer_source   TEXT NOT NULL,      -- 'phone' or 'button'
    ack_missing     INTEGER NOT NULL DEFAULT 0,
    answered_mono   INTEGER NOT NULL,
    answered_wall   TEXT NOT NULL,
    FOREIGN KEY (session_token) REFERENCES sessions(token)
);

CREATE TABLE IF NOT EXISTS items (
    id          TEXT PRIMARY KEY,
    channel     TEXT NOT NULL,
    label       INTEGER NOT NULL,       -- 0 safe, 1 phish
    difficulty  TEXT NOT NULL,
    cues        TEXT NOT NULL           -- JSON array
);

CREATE TABLE IF NOT EXISTS tap_log (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    -- card_ref, not the raw UID. See the note on sessions above.
    card_ref    TEXT NOT NULL,
    player_id   TEXT,
    session_token TEXT,
    t_mono      INTEGER NOT NULL,
    t_wall      TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_sessions_player ON sessions(player_id);
CREATE INDEX IF NOT EXISTS idx_sessions_card ON sessions(card_ref);
CREATE INDEX IF NOT EXISTS idx_answers_session ON answers(session_token);
CREATE INDEX IF NOT EXISTS idx_answers_player ON answers(player_id);
CREATE INDEX IF NOT EXISTS idx_tap_log_card ON tap_log(card_ref);
"""


def _init_schema(conn: sqlite3.Connection) -> None:
    """Create the schema if it does not exist. Idempotent."""
    global _schema_initialised
    if _schema_initialised:
        return
    with _schema_lock:
        if _schema_initialised:
            return
        conn.executescript(SCHEMA_SQL)
        # Record the schema version and the creation time for future migrations.
        conn.execute(
            "INSERT OR IGNORE INTO meta(key, value) VALUES (?, ?)",
            ("schema_version", "1"),
        )
        conn.execute(
            "INSERT OR IGNORE INTO meta(key, value) VALUES (?, ?)",
            ("created_wall", wall_now_iso()),
        )
        _schema_initialised = True


@contextmanager
def transaction() -> Iterator[sqlite3.Connection]:
    """A context manager that yields the connection and commits on success.

    In autocommit mode each statement is its own transaction, but grouping
    several statements inside BEGIN/COMMIT is still useful for atomicity: a
    session creation that writes to both ``sessions`` and ``tap_log`` should
    be all-or-nothing.
    """
    conn = db()
    conn.execute("BEGIN")
    try:
        yield conn
        conn.execute("COMMIT")
    except Exception:
        conn.execute("ROLLBACK")
        raise


def now_mono() -> int:
    """Convenience: monotonic milliseconds, for INSERT statements."""
    return monotonic_ms()


def now_wall() -> str:
    """Convenience: wall-clock ISO string, for INSERT statements."""
    return wall_now_iso()
