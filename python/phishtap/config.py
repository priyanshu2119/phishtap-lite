"""
PhishTap Lite — configuration.

Every tunable in one place, each with its reasoning. Mirrors the philosophy of
sketch/phishtap/pt_config.h: a magic number with no stated reason is a number
nobody dares change later.

Two modes drive the whole backend:

  * ``IDENTITY_MODE = "shared_card"``  (demo default)
    One card is shared by several people, so the UID alone collapses every
    player into a single row. The nickname is folded into the HMAC identity
    bytes to keep them distinct.

  * ``IDENTITY_MODE = "per_card"``  (production)
    Each student owns a card, so the UID alone is enough and stays distinct
    even if two people pick the same nickname.

See docs/ARCHITECTURE.md §8 for the full identity design.
"""

from __future__ import annotations

import os
from pathlib import Path

# ── Protocol ──────────────────────────────────────────────────────────────
# Must match PT_PROTO_VERSION in sketch/phishtap/pt_config.h. Bump on any
# change to the shape of a bridge payload.
PROTO_VERSION: int = 1
FW_VERSION: str = "1b.1"

# ── Identity ──────────────────────────────────────────────────────────────
IDENTITY_MODE: str = os.environ.get("PT_IDENTITY_MODE", "shared_card")

# ── Paths ─────────────────────────────────────────────────────────────────
# On the board, /app is the container root and the app folder is bind-mounted
# there. On a laptop, the repo root stands in. Resolved once at import so every
# module agrees about where the database and the secret live.
_APP_ROOT = Path(os.environ.get("APP_ROOT", Path(__file__).resolve().parent.parent.parent))
DATA_DIR: Path = _APP_ROOT / "data"
ASSETS_DIR: Path = _APP_ROOT / "assets"
CORPUS_PATH: Path = _APP_ROOT / "python" / "corpus.json"

DB_PATH: Path = DATA_DIR / "phishtap.db"
SERVER_SECRET_PATH: Path = DATA_DIR / "server_secret.key"
ADMIN_PIN_PATH: Path = DATA_DIR / "admin_pin"

# ── Session timing ────────────────────────────────────────────────────────
# A session token expires if no phone connects within this window, and again at
# game end. 90 s is long enough to scan a QR code and type a nickname, short
# enough that a student who walks away does not leave a dangling session.
SESSION_TTL_S: float = 90.0

# The same card cannot start a new session within this window. Must match
# PT_CARD_COOLDOWN_MS (3000) on the MCU, or the two sides will disagree about
# what counts as a re-tap.
CARD_COOLDOWN_S: float = 3.0

# A review token is valid for this long after game over, so a student can go
# back and look at what they got wrong without the token expiring mid-read.
REVIEW_TOKEN_TTL_S: float = 600.0

# ── Game rules ────────────────────────────────────────────────────────────
# Round time is max(4, 14 - 0.8 * round_index) seconds. The floor and ceiling
# here must match PT_ROUND_LIMIT_MIN_MS / PT_ROUND_LIMIT_MAX_MS on the MCU
# (4000 / 14000), or pt.start_round will reject the call.
ROUND_LIMIT_MIN_S: float = 4.0
ROUND_LIMIT_MAX_S: float = 14.0
ROUND_DECAY: float = 0.8
ROUND_INITIAL_S: float = 14.0

STARTING_LIVES: int = 3

# ── Rate limiting ─────────────────────────────────────────────────────────
# In-memory token buckets, per IP and per token. They survive nothing, which is
# the correct failure mode: a restart clearing the buckets is the right thing,
# because a restart also clears the in-flight sessions that the buckets were
# protecting.
RATE_CONNECT_PER_MIN: int = 10
RATE_JOIN_PER_MIN: int = 10
RATE_ANSWER_PER_ROUND: int = 3
RATE_ADMIN_LOGIN_PER_MIN: int = 5
JOIN_LOCKOUT_THRESHOLD: int = 5

# ── Bridge polling ────────────────────────────────────────────────────────
# How often the bridge adapter polls pt.get_events. 100 ms is ten times a second,
# which drains the 16-slot ring far faster than anything can fill it. Slower
# risks losing events to overflow under load; faster wastes CPU and bridge
# bandwidth for no perceptible latency gain.
BRIDGE_POLL_INTERVAL_S: float = 0.1

# How often to call pt.get_status as a heartbeat, independent of event polling.
# The MCU treats any RPC as proof of link liveness, so event polling alone keeps
# the link up; this is a separate health read for the kiosk display.
BRIDGE_STATUS_INTERVAL_S: float = 1.0

# Bridge.call timeout. The MCU responds in well under a millisecond when loop()
# is non-blocking, so 5 s is a generous ceiling that still fails fast enough to
# be useful if the MCU has genuinely hung.
BRIDGE_CALL_TIMEOUT_S: float = 5.0

# ── Link-loss ─────────────────────────────────────────────────────────────
# If no MCU heartbeat is seen for this long, the kiosk shows a hardware-fault
# banner and the game pauses. Must be shorter than the MCU's PT_LINK_LOSS_MS
# (15000) so Python notices first and can tell the user, rather than the MCU
# giving up and going to link-lost while Python still thinks everything is fine.
LINK_LOSS_S: float = 5.0

# ── Web UI ────────────────────────────────────────────────────────────────
# The WebUI brick defaults to 0.0.0.0:7000. Stated here so the kiosk and the
# phone client know where to connect without hardcoding it in two places.
WEB_PORT: int = 7000

# ── Nickname validation ───────────────────────────────────────────────────
NICKNAME_MIN_LEN: int = 3
NICKNAME_MAX_LEN: int = 12

# ── Admin ─────────────────────────────────────────────────────────────────
# The admin PIN is generated on first boot and stored mode 0600. A reset
# requires the literal string RESET, not just any confirmation, because a
# destructive operation should not be triggerable by a typo.
ADMIN_PIN_LENGTH: int = 6
DB_RESET_CONFIRM: str = "RESET"


def ensure_data_dir() -> None:
    """Create the data directory if it does not exist.

    Called once at startup. The directory holds the SQLite database, the
    server secret and the admin PIN. On the board it persists across container
    restarts because /app is a bind mount; on a laptop it is a local folder.
    """
    DATA_DIR.mkdir(parents=True, exist_ok=True)
