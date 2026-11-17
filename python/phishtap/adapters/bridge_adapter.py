"""
PhishTap Lite — real bridge adapter.

THE ONLY FILE IN THIS PROJECT THAT IMPORTS ``arduino.app_utils``. That is a
deliberate, enforced constraint (see ``tests/test_layering.py``): it is what
makes the rest of the backend runnable on a laptop with no board attached.

The import is LAZY. Importing this module on a laptop must not explode with a
bare ``ModuleNotFoundError``, because the layering test imports it to check that
nothing else does. So the arduino import happens inside ``__init__`` and a
missing module becomes a clear ``BridgeError`` naming the actual problem.

CALL SEMANTICS, verified against the real package inside
``ghcr.io/arduino/app-bricks/python-apps-base:0.13.2``:

    Bridge.call(method_name: str, *params: object, timeout: float | None = 10) -> object

Synchronous, returns the decoded MsgPack value. Our MCU handlers return a single
JSON string, so the result is a ``str`` that needs ``json.loads``. The code also
accepts an already-decoded dict, because the underlying router could in principle
hand back a structured value and failing on that would be a needless crash.

Errors, per the docstring in ``arduino/app_utils/bridge.py``:
    ValueError        the peer answered with an error (e.g. method not registered);
                      wraps an ``arduino.router_bridge.RpcError`` with .code/.message
    TimeoutError      the call exceeded the timeout
    ConnectionError   the connection dropped while waiting
    RuntimeError      called from inside a provide handler, or an unexpected failure

All four are translated into ``BridgeError`` or ``LinkDown`` so the engine has one
thing to catch and does not need to know which library raised what.
"""

from __future__ import annotations

import json
import logging
import threading
from typing import Any

from ..bridge_port import BridgeError, LinkDown
from ..config import BRIDGE_CALL_TIMEOUT_S, PROTO_VERSION

logger = logging.getLogger("phishtap.bridge")


class RealBridge:
    """Talks to the MCU over the App Lab bridge.

    Satisfies ``BridgePort``.

    A single lock serialises every outbound call. Concurrent ``Bridge.call``
    from multiple threads is *probably* safe — the underlying MsgPack-RPC
    correlates requests by id — but it is not documented as safe, and the cost of
    serialising is negligible at ten polls a second. Guessing wrong here would
    produce interleaved responses that are extremely hard to diagnose, so the
    conservative choice is the correct one.
    """

    def __init__(self, *, timeout: float = BRIDGE_CALL_TIMEOUT_S) -> None:
        self._timeout = timeout
        self._call_lock = threading.Lock()
        self._bridge = None
        self._import_error: str | None = None

        try:
            from arduino.app_utils import Bridge as _Bridge  # noqa: PLC0415
            self._bridge = _Bridge
        except ImportError as e:
            # Recorded, not raised. The layering test imports this module on a
            # laptop where arduino.app_utils genuinely does not exist, and that
            # import must succeed for the test to run at all.
            self._import_error = str(e)

    # ── Internals ─────────────────────────────────────────────────────────

    def _ensure_available(self) -> None:
        if self._bridge is None:
            raise LinkDown(
                "arduino.app_utils is not importable, so the real bridge cannot "
                f"be used ({self._import_error}). This adapter only works inside "
                "an App Lab container; use MockBridge for laptop development."
            )

    def _call(self, method: str, payload: dict[str, Any] | None = None) -> Any:
        """Issue one RPC and decode the JSON response.

        ``payload`` is serialised to a JSON string because that is the wire
        format the firmware expects: every MCU handler takes a single JSON string
        and returns a single JSON string.
        """
        self._ensure_available()
        arg = json.dumps(payload if payload is not None else {})

        try:
            with self._call_lock:
                raw = self._bridge.call(method, arg, timeout=self._timeout)
        except TimeoutError as e:
            # A timeout means the MCU did not answer in time. That is a link
            # problem, not a rejected call, so it maps to LinkDown: the kiosk
            # should show a hardware fault and the game should pause.
            raise LinkDown(f"{method} timed out after {self._timeout:g}s") from e
        except ConnectionError as e:
            raise LinkDown(f"connection to the MCU dropped during {method}") from e
        except ValueError as e:
            # The peer answered with an RPC error. Most commonly the method is
            # not registered, which means the firmware on the board is older than
            # this backend, or the sketch never finished booting.
            raise BridgeError(f"{method} was rejected by the MCU: {e}") from e
        except RuntimeError as e:
            # Raised when call() is invoked from inside a provide handler, which
            # would deadlock. This backend never registers provide handlers that
            # call back, so hitting this means that rule was broken.
            raise BridgeError(f"{method} failed: {e}") from e

        return self._decode(method, raw)

    @staticmethod
    def _decode(method: str, raw: Any) -> Any:
        """Turn the bridge's return value into a Python object."""
        if raw is None:
            raise BridgeError(f"{method} returned no response")
        if isinstance(raw, (dict, list)):
            return raw
        if isinstance(raw, (bytes, bytearray)):
            raw = raw.decode("utf-8", errors="replace")
        if not isinstance(raw, str):
            raise BridgeError(
                f"{method} returned an unexpected type {type(raw).__name__}"
            )
        try:
            return json.loads(raw)
        except json.JSONDecodeError as e:
            # The firmware builds JSON into a fixed buffer and returns 0 on
            # overflow, in which case it replies with an explicit error object
            # rather than a truncated document. So a decode failure here means
            # something unexpected happened and is worth the raw text.
            raise BridgeError(
                f"{method} returned invalid JSON: {e}; raw={raw[:200]!r}"
            ) from e

    # ── Reads ─────────────────────────────────────────────────────────────

    def get_status(self) -> dict[str, Any]:
        data = self._call("pt.get_status")
        if not isinstance(data, dict):
            raise BridgeError(f"pt.get_status returned {type(data).__name__}, expected object")

        proto = data.get("proto")
        if proto is not None and proto != PROTO_VERSION:
            # Not fatal, but loud. A protocol mismatch means the firmware and the
            # backend were built from different commits, and every field after
            # this point is suspect. Logging it once per call is acceptable
            # because get_status is polled about once a second.
            logger.warning(
                "MCU protocol version %s does not match backend %s; "
                "the firmware and the Python side are out of sync",
                proto, PROTO_VERSION,
            )
        return data

    def get_events(self, since: int) -> tuple[list[dict[str, Any]], int, bool]:
        data = self._call("pt.get_events", {"since": int(since)})
        if not isinstance(data, dict):
            raise BridgeError(f"pt.get_events returned {type(data).__name__}, expected object")

        events = data.get("events")
        if not isinstance(events, list):
            raise BridgeError("pt.get_events response has no 'events' array")

        oldest = data.get("oldest_seq", 0)
        overflow = bool(data.get("overflow", False))
        return events, int(oldest), overflow

    def selftest(self) -> dict[str, Any]:
        data = self._call("pt.selftest")
        if not isinstance(data, dict):
            raise BridgeError(f"pt.selftest returned {type(data).__name__}, expected object")
        return data

    # ── Writes ────────────────────────────────────────────────────────────

    def ack_events(self, seq: int) -> None:
        self._call("pt.ack_events", {"seq": int(seq)})

    def set_state(self, state: str) -> None:
        self._check_ok("pt.set_state", self._call("pt.set_state", {"state": state}))

    def start_round(self, round_index: int, limit_ms: int) -> None:
        self._check_ok(
            "pt.start_round",
            self._call(
                "pt.start_round",
                {"round": int(round_index), "limit_ms": int(limit_ms)},
            ),
        )

    def end_round(self, result: str) -> None:
        self._check_ok("pt.end_round", self._call("pt.end_round", {"result": result}))

    def set_lives(self, n: int) -> None:
        self._check_ok("pt.set_lives", self._call("pt.set_lives", {"n": int(n)}))

    def show_score(self, score: int) -> None:
        self._check_ok("pt.show_score", self._call("pt.show_score", {"score": int(score)}))

    def buzz(self, pattern: str) -> None:
        self._check_ok("pt.buzz", self._call("pt.buzz", {"pattern": pattern}))

    # ── Response checking ─────────────────────────────────────────────────

    @staticmethod
    def _check_ok(method: str, data: Any) -> None:
        """Raise if the MCU answered ``{"ok": false, "err": ...}``.

        The firmware rejects a malformed or out-of-range request rather than
        clamping it, so a false here is a real bug in this backend and must not
        be swallowed. Surfacing it as an exception means the first time it
        happens is in a test or in the log, not as a silently ignored command.
        """
        if not isinstance(data, dict):
            raise BridgeError(f"{method} returned {type(data).__name__}, expected object")
        if data.get("ok") is False:
            raise BridgeError(f"{method} was refused by the MCU: {data.get('err')}")
