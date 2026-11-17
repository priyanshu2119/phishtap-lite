"""
PhishTap Lite — App Lab entry point.

THIN BY DESIGN. This file wires components together and starts the loop. It
contains no game logic, no policy and no SQL, because anything written here is
unreachable from the laptop test suite: ``main.py`` only runs inside the App Lab
container, where ``arduino.app_utils`` exists and a board is attached.

Everything testable lives in the ``phishtap`` package. Everything here is either
construction order, environment selection, or the polling loop.

RUNTIME LAYOUT (verified from the container's /run.sh):
    cd /app && exec python /app/python/main.py
with requirements installed by uv into /app/.cache/.venv and the prepare phase
running ``python -m compileall`` over /app/python first, so a syntax error fails
the build rather than crashing at runtime.
"""

from __future__ import annotations

import logging
import os
import sys
import time

# ── Logging ───────────────────────────────────────────────────────────────
# Configured before anything else imports, so no module can emit a record before
# the UID redaction filter is attached. Order matters here: a log line written
# during import of phishtap.engine would bypass a filter installed afterwards.

logging.basicConfig(
    level=os.environ.get("PT_LOG_LEVEL", "INFO").upper(),
    format="%(asctime)s %(levelname)-5s [%(name)s] %(message)s",
    stream=sys.stdout,
)
logger = logging.getLogger("phishtap.main")

from phishtap.clock import ClockOffset                        # noqa: E402
from phishtap.config import (                                 # noqa: E402
    ASSETS_DIR,
    BRIDGE_POLL_INTERVAL_S,
    BRIDGE_STATUS_INTERVAL_S,
    CORPUS_PATH,
    WEB_PORT,
    ensure_data_dir,
)
from phishtap.corpus import CorpusError, load_corpus          # noqa: E402
from phishtap.db import db                                    # noqa: E402
from phishtap.engine import GameEngine                        # noqa: E402
from phishtap.api import AdminAuth, ApiService                # noqa: E402
from phishtap.privacy import install_uid_redaction            # noqa: E402
from phishtap.ratelimit import JoinAttempts, RateLimiter      # noqa: E402
from phishtap.sessions import SessionStore                    # noqa: E402


def _build_bridge():
    """Return the real bridge, or the mock if arduino.app_utils is unavailable.

    Falling back rather than crashing is what lets the same ``main.py`` run on a
    laptop for a frontend demo with no board attached. The fallback is logged
    loudly, because a console silently running against a mock during a real demo
    would look like working hardware while reading no cards at all.
    """
    if os.environ.get("PT_FORCE_MOCK_BRIDGE") == "1":
        logger.warning("PT_FORCE_MOCK_BRIDGE=1, using the mock bridge")
        from phishtap.adapters.mock_bridge import MockBridge
        return MockBridge(), False

    from phishtap.adapters.bridge_adapter import RealBridge
    bridge = RealBridge()
    try:
        # A cheap call that fails immediately if arduino.app_utils is missing or
        # the router is unreachable, so the decision is made here rather than on
        # the first card tap in front of an audience.
        bridge.get_status()
        logger.info("connected to the MCU bridge")
        return bridge, True
    except Exception as e:  # noqa: BLE001
        logger.warning(
            "MCU bridge unavailable (%s). Falling back to the MOCK bridge: the web "
            "UI will work but no cards will be read. Set PT_FORCE_MOCK_BRIDGE=1 to "
            "silence this warning when running off-board on purpose.", e,
        )
        from phishtap.adapters.mock_bridge import MockBridge
        return MockBridge(), False


def main() -> None:
    install_uid_redaction()
    ensure_data_dir()

    logger.info("PhishTap Lite backend starting")
    logger.info("  app root : %s", os.environ.get("APP_ROOT", "(default)"))
    logger.info("  corpus   : %s", CORPUS_PATH)
    logger.info("  database : %s", os.environ.get("PT_DB_PATH", "(default data/)"))

    # ── Corpus ────────────────────────────────────────────────────────────
    # Loaded before anything else, and a failure is fatal. A console that starts
    # with an empty corpus accepts taps, creates sessions and then ends every game
    # on round zero, which looks like a bug in the engine rather than a missing
    # content file.
    try:
        items = load_corpus()
    except CorpusError as e:
        logger.error("corpus failed to load: %s", e)
        logger.error("refusing to start; fix %s and retry", CORPUS_PATH)
        raise SystemExit(1) from e
    logger.info("  corpus   : %d items loaded", len(items))
    if len(items) < 8:
        logger.warning(
            "only %d corpus items; sessions will be very short. Stage 4 targets 40.",
            len(items),
        )

    # ── Database ──────────────────────────────────────────────────────────
    # Touching the connection here creates the schema eagerly, so a permissions
    # problem surfaces at startup with a clear message instead of on the first
    # card tap.
    try:
        db()
    except Exception as e:  # noqa: BLE001
        logger.error("could not open the database: %s", e)
        raise SystemExit(1) from e
    logger.info("  database : schema ready")

    # ── Clock offset ──────────────────────────────────────────────────────
    # Restored from meta so an admin's correction survives a restart. The board
    # has no RTC, so without this every reboot resets the wall clock to whatever
    # the container thinks and the exported timestamps drift.
    clock = ClockOffset()
    row = db().execute("SELECT value FROM meta WHERE key='clock_offset_ms'").fetchone()
    if row is not None:
        try:
            clock.set(int(row["value"]))
            logger.info("  clock    : restored offset %d ms", clock.offset_ms)
        except (TypeError, ValueError):
            logger.warning("stored clock_offset_ms is not an integer, ignoring")

    # ── Bridge ────────────────────────────────────────────────────────────
    bridge, bridge_real = _build_bridge()

    # ── Web UI ────────────────────────────────────────────────────────────
    # Constructed before the engine because the engine needs the notifier, and
    # the notifier needs the brick. api.base_url is set from ui.url, which reads
    # the HOST_IP environment variable App Lab injects; that is what makes the QR
    # code point at an address a phone on the same network can actually reach.
    from phishtap.adapters.web_adapter import WebNotifier, create_web_ui, register_web

    use_tls = os.environ.get("PT_USE_TLS") == "1"
    ui = create_web_ui(
        port=WEB_PORT,
        assets_dir=str(ASSETS_DIR) if ASSETS_DIR.exists() else "/app/assets",
        use_tls=use_tls,
    )
    notifier = WebNotifier(ui)
    logger.info("  web ui   : %s (tls=%s)", ui.url, use_tls)

    # ── Engine and API ────────────────────────────────────────────────────
    store = SessionStore()
    engine = GameEngine(bridge, store, items, notifier)
    admin = AdminAuth()
    api = ApiService(
        engine,
        limiter=RateLimiter(),
        join_attempts=JoinAttempts(),
        admin=admin,
        clock=clock,
        base_url=ui.url,
    )
    register_web(ui, api)

    # Print the admin PIN once at startup. It is stored in data/admin_pin and is
    # not recoverable from the UI, so a first run has to surface it somehow. It
    # goes to the Python tab of the App Lab console, which is not public.
    logger.info("  admin PIN: %s  (stored in data/admin_pin)", admin.get_or_create_pin())
    logger.info("  bridge   : %s", "real MCU" if bridge_real else "MOCK (no hardware)")

    # ── Main loop ─────────────────────────────────────────────────────────
    # Polling runs here on the main thread rather than in a background thread.
    # App.run calls user_loop in a tight while True, and the WebUI brick already
    # runs its own uvicorn thread, so there is no need for a second thread and one
    # fewer place for a race to live.
    last_status = 0.0

    def loop() -> None:
        nonlocal last_status
        engine.poll()

        now = time.monotonic()
        if now - last_status >= BRIDGE_STATUS_INTERVAL_S:
            last_status = now
            engine.refresh_status()
            # Push the kiosk state on a timer rather than only on events, so a
            # browser that connects mid-game still sees the current state without
            # having to poll over REST.
            notifier.to_kiosk("kiosk_state", engine.kiosk_snapshot())

        time.sleep(BRIDGE_POLL_INTERVAL_S)

    logger.info("======== PhishTap Lite ready ========")
    logger.info("kiosk: %s/index.html", ui.url)

    from arduino.app_utils import App
    App.run(user_loop=loop)


if __name__ == "__main__":
    main()
