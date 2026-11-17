"""
Shared pytest fixtures.

DATABASE ISOLATION
``phishtap.config`` resolves every path from ``APP_ROOT`` at import time, so the
environment variable has to be set BEFORE any phishtap module is imported. pytest
imports conftest.py before collecting test modules, which makes this the one place
that can do it reliably. Setting it inside a fixture would be too late: the config
module would already have cached the real repo paths.

The temp directory is module-scoped rather than function-scoped because creating
a directory per test is wasted work; tables are truncated between tests instead,
which is both faster and closer to what the app actually does at runtime.
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

# Must happen before any `phishtap` import below.
_TMP_ROOT = Path(tempfile.mkdtemp(prefix="phishtap-test-"))
os.environ["APP_ROOT"] = str(_TMP_ROOT)
# Never let a test talk to a real board, even if one happens to be attached.
os.environ.setdefault("PT_FORCE_MOCK_BRIDGE", "1")

import pytest  # noqa: E402

from phishtap import config  # noqa: E402
from phishtap.db import db  # noqa: E402


@pytest.fixture(scope="session", autouse=True)
def _app_root() -> Path:
    """Expose the temp app root, and confirm the paths resolved to it."""
    assert config.DATA_DIR == _TMP_ROOT / "data"
    assert config.CORPUS_PATH.name == "corpus.json"
    config.ensure_data_dir()
    return _TMP_ROOT


@pytest.fixture(autouse=True)
def clean_db():
    """Truncate every table before each test.

    Runs before AND after, so a test that fails halfway does not leave rows that
    make the next test fail for an unrelated reason. That cascade is what makes
    flaky suites untrustworthy, and an untrustworthy suite gets ignored.
    """
    _truncate()
    yield
    _truncate()


def _truncate() -> None:
    conn = db()
    # Order matters: children before parents, because foreign_keys is ON.
    for table in ("answers", "tap_log", "sessions", "players", "items"):
        conn.execute(f"DELETE FROM {table}")  # noqa: S608 - fixed table names


@pytest.fixture
def real_corpus():
    """The repository corpus, resolved from this file's location.

    ``config.CORPUS_PATH`` points into the temp APP_ROOT, where no corpus exists.
    Tests that need real content use this instead, which walks up from the tests
    directory to the repository. That keeps the tests honest about the fact that
    the corpus is a repository artefact, not runtime data.
    """
    from phishtap.corpus import load_corpus

    repo_root = Path(__file__).resolve().parent.parent
    return load_corpus(repo_root / "python" / "corpus.json")


@pytest.fixture
def bridge():
    from phishtap.adapters.mock_bridge import MockBridge

    return MockBridge()


@pytest.fixture
def store():
    from phishtap.sessions import SessionStore

    return SessionStore()


class RecordingNotifier:
    """Captures notifications so tests can assert on what the browser was told."""

    def __init__(self) -> None:
        self.rooms: list[tuple[str, str, dict]] = []
        self.kiosk: list[tuple[str, dict]] = []

    def to_room(self, room: str, event: str, payload: dict) -> None:
        self.rooms.append((room, event, payload))

    def to_kiosk(self, event: str, payload: dict) -> None:
        self.kiosk.append((event, payload))

    def events_for(self, name: str) -> list[dict]:
        return [p for _, e, p in self.rooms if e == name]

    def kiosk_events(self, name: str) -> list[dict]:
        return [p for e, p in self.kiosk if e == name]


@pytest.fixture
def notifier():
    return RecordingNotifier()


@pytest.fixture
def engine(bridge, store, real_corpus, notifier):
    """An engine with a seeded RNG, so item order is reproducible across runs."""
    import random

    from phishtap.engine import GameEngine

    return GameEngine(bridge, store, real_corpus, notifier, rng=random.Random(1234))


@pytest.fixture
def api(engine):
    from phishtap.api import AdminAuth, ApiService
    from phishtap.ratelimit import JoinAttempts, RateLimiter

    return ApiService(
        engine,
        limiter=RateLimiter(),
        join_attempts=JoinAttempts(),
        admin=AdminAuth(),
        base_url="http://192.168.1.50:7000",
    )
