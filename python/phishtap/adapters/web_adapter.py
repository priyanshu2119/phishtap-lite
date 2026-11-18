"""
PhishTap Lite — web adapter.

THE ONLY FILE IN THIS PROJECT THAT IMPORTS ``arduino.app_bricks``. Together with
``bridge_adapter.py`` (the only file importing ``arduino.app_utils``) these are
the two thin shells the layering rule in docs/ARCHITECTURE.md §1 allows.

This module does three things and nothing else:

  1. constructs the ``WebUI`` brick
  2. implements ``Notifier`` by forwarding to ``send_message``
  3. wires HTTP requests to ``ApiService`` methods and formats the responses

All policy lives in ``api.py``. A handler here should be readable in one pass and
contain no ``if`` that expresses a business rule, because a rule buried in an
adapter is a rule no test covers.

VERIFIED AGAINST THE REAL BRICK, not guessed. The constructor signature, the
``expose_api`` / ``on_message`` / ``send_message`` shapes, the fact that
``use_tls`` is correct while ``use_ssl`` is a deprecated alias, and the presence
of built-in ``enter_room`` / ``leave_room`` Socket.IO events were all read out of
``ghcr.io/arduino/app-bricks/python-apps-base:0.13.2`` on the board.
"""

from __future__ import annotations

import logging
from typing import Any, Callable

from fastapi import Header, Request
from fastapi.responses import JSONResponse, Response

from ..api import ApiService

logger = logging.getLogger("phishtap.web")

KIOSK_ROOM = "kiosk"


class WebNotifier:
    """Implements the engine's ``Notifier`` protocol over Socket.IO.

    ``send_message`` is documented as thread-safe (it schedules the emit with
    ``asyncio.run_coroutine_threadsafe``), which is what lets the engine call it
    from the bridge polling thread rather than only from a request handler.
    """

    def __init__(self, ui: Any) -> None:
        self._ui = ui

    def to_room(self, room: str, event: str, payload: dict[str, Any]) -> None:
        try:
            self._ui.send_message(event, payload, room=room)
        except Exception:  # noqa: BLE001
            # A browser that vanished mid-emit must not take down the game loop.
            # The engine has already committed the state change; only the
            # notification failed, and the client will resync on its next poll.
            logger.exception("failed to emit %s to room %s", event, room)

    def to_kiosk(self, event: str, payload: dict[str, Any]) -> None:
        self.to_room(KIOSK_ROOM, event, payload)


def _client_ip(request: Request) -> str:
    """Best-effort client address for rate limiting.

    Falls back to a constant when the address is unavailable, so rate limiting
    degrades to "one shared bucket" rather than crashing. That is the safe
    direction to fail: an attacker cannot get unlimited attempts, they can only
    share a bucket with everyone else.
    """
    client = getattr(request, "client", None)
    host = getattr(client, "host", None) if client is not None else None
    return host or "unknown"


async def _read_inputs(request: Request) -> dict[str, Any]:
    """Merge the JSON body and the query parameters, body winning on conflict.

    WHY THIS FUNCTION EXISTS, AND THE BUG IT REPLACED
    Every POST handler used to be declared like this:

        def post_connect(request: Request, token: str = "") -> JSONResponse:

    That looks like it reads a JSON body. It does not. In FastAPI a SCALAR
    parameter with a default is interpreted as a QUERY parameter; only a Pydantic
    model (or an explicit Body(...)) is read from the body. So a phone posting
    {"token": "abc"} was silently ignored, `token` took its default "", and the
    API answered "token is required" for a request that had just supplied one.

    Every POST endpoint was broken this way, and 351 passing tests did not catch
    it, because they all called ApiService methods directly and never went
    through HTTP. The bug only appeared when a real browser scanned a real QR
    code. tests/test_web_adapter.py now issues real requests so it cannot recur.

    Accepting the body OR the query string is deliberate rather than merely
    forgiving: the browser sends a body, and a query string keeps `curl -X POST
    .../?token=x` working for whoever is debugging at midnight. The body wins, so
    a client cannot be overridden by a stray query parameter.

    A missing or non-JSON body is not an error. It yields an empty dict, and the
    API layer then returns its own precise E_* code, which is a far more useful
    answer than a 422 from the framework.
    """
    out: dict[str, Any] = dict(request.query_params)
    try:
        body = await request.json()
    except Exception:  # noqa: BLE001 - no body, empty body, or not JSON
        return out
    if isinstance(body, dict):
        out.update(body)
    return out


def _str(inputs: dict[str, Any], key: str) -> str:
    """Read a string field, tolerating a JSON null or a non-string.

    A client sending {"token": null} must produce the same "token is required"
    error as omitting it, not a TypeError from deep inside the API layer.
    """
    value = inputs.get(key)
    if value is None:
        return ""
    return value if isinstance(value, str) else str(value)


def _int(inputs: dict[str, Any], key: str, default: int) -> int:
    """Read an integer field, falling back to ``default`` on anything unusable."""
    value = inputs.get(key, default)
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def create_web_ui(
    *,
    port: int = 7000,
    assets_dir: str = "/app/assets",
    use_tls: bool = False,
) -> Any:
    """Construct the WebUI brick.

    ``use_tls`` and not ``use_ssl``: the latter is a deprecated alias that logs a
    warning. Confirmed against the brick's own test suite, which asserts that
    ``WebUI(use_ssl=True)`` sets ``_use_tls``.
    """
    from arduino.app_bricks.web_ui import WebUI  # noqa: PLC0415 - lazy by design

    return WebUI(port=port, assets_dir_path=assets_dir, use_tls=use_tls)


def register_web(ui: Any, api: ApiService) -> None:
    """Attach every REST route and WebSocket handler to the brick."""
    _register_rest(ui, api)
    _register_ws(ui, api)


# ── REST ──────────────────────────────────────────────────────────────────
#
# Every handler extracts its inputs, calls one ApiService method, and formats the
# result. Query strings only, no path parameters: docs/ARCHITECTURE.md §7 notes
# that path-parameter support was unverified, and there is no reason to depend on
# it when a query string does the same job.

def _register_rest(ui: Any, api: ApiService) -> None:

    def as_json(result: tuple[int, dict[str, Any]]) -> JSONResponse:
        status, payload = result
        return JSONResponse(status_code=status, content=payload)

    # -- public -----------------------------------------------------------

    def health() -> JSONResponse:
        return as_json(api.health())

    def kiosk() -> JSONResponse:
        return as_json(api.kiosk())

    def get_session(token: str = "") -> JSONResponse:
        return as_json(api.get_session(token))

    def leaderboard() -> JSONResponse:
        return as_json(api.leaderboard())

    def get_review(token: str = "") -> JSONResponse:
        return as_json(api.review(token))

    async def post_connect(request: Request) -> JSONResponse:
        inputs = await _read_inputs(request)
        return as_json(api.connect(_str(inputs, "token"), _client_ip(request)))

    async def post_join(request: Request) -> JSONResponse:
        inputs = await _read_inputs(request)
        return as_json(api.join(_str(inputs, "pin"), _client_ip(request)))

    async def post_consent(request: Request) -> JSONResponse:
        inputs = await _read_inputs(request)
        return as_json(api.consent(
            _str(inputs, "token"),
            _str(inputs, "nickname"),
            _int(inputs, "consent_version", 1),
            _client_ip(request),
        ))

    async def post_render_ack(request: Request) -> JSONResponse:
        inputs = await _read_inputs(request)
        return as_json(api.render_ack(_str(inputs, "token")))

    async def post_answer(request: Request) -> JSONResponse:
        inputs = await _read_inputs(request)
        return as_json(api.answer(
            _str(inputs, "token"),
            _str(inputs, "answer"),
            _str(inputs, "source") or "phone",
            _client_ip(request),
        ))

    async def post_delete_request(request: Request) -> JSONResponse:
        inputs = await _read_inputs(request)
        return as_json(api.delete_request(_str(inputs, "token")))

    # -- admin ------------------------------------------------------------
    #
    # The admin token travels in a header, not a query string. Credentials in a
    # URL end up in access logs, browser history and Referer headers, which is a
    # needless way to leak a token that grants database deletion.

    async def admin_login(request: Request) -> JSONResponse:
        inputs = await _read_inputs(request)
        return as_json(api.admin_login(_str(inputs, "pin"), _client_ip(request)))

    def admin_export(
        x_admin_token: str | None = Header(default=None, alias="X-Admin-Token"),
    ) -> Response:
        status, content_type, body = api.admin_export(x_admin_token)
        if content_type.startswith("application/json"):
            return Response(status_code=status, content=body, media_type=content_type)
        return Response(
            status_code=status,
            content=body,
            media_type=content_type,
            headers={"Content-Disposition": 'attachment; filename="phishtap-export.csv"'},
        )

    async def admin_delete_player(
        request: Request,
        x_admin_token: str | None = Header(default=None, alias="X-Admin-Token"),
    ) -> JSONResponse:
        inputs = await _read_inputs(request)
        return as_json(api.admin_delete_player(x_admin_token, _str(inputs, "player_id")))

    async def admin_db_reset(
        request: Request,
        x_admin_token: str | None = Header(default=None, alias="X-Admin-Token"),
    ) -> JSONResponse:
        inputs = await _read_inputs(request)
        return as_json(api.admin_db_reset(x_admin_token, _str(inputs, "confirm")))

    async def admin_clock(
        request: Request,
        x_admin_token: str | None = Header(default=None, alias="X-Admin-Token"),
    ) -> JSONResponse:
        inputs = await _read_inputs(request)
        return as_json(api.admin_clock(x_admin_token, _int(inputs, "offset_ms", 0)))

    def admin_analytics(
        x_admin_token: str | None = Header(default=None, alias="X-Admin-Token"),
    ) -> JSONResponse:
        return as_json(api.admin_analytics(x_admin_token))

    routes: list[tuple[str, str, Callable[..., Any]]] = [
        ("GET",  "/api/health",              health),
        ("GET",  "/api/kiosk",               kiosk),
        ("GET",  "/api/session",             get_session),
        ("GET",  "/api/leaderboard",         leaderboard),
        ("GET",  "/api/review",              get_review),
        ("POST", "/api/session/connect",     post_connect),
        ("POST", "/api/join",                post_join),
        ("POST", "/api/consent",             post_consent),
        ("POST", "/api/render_ack",          post_render_ack),
        ("POST", "/api/answer",              post_answer),
        ("POST", "/api/delete_request",      post_delete_request),
        ("POST", "/api/admin/login",         admin_login),
        ("GET",  "/api/admin/export",        admin_export),
        ("POST", "/api/admin/player/delete", admin_delete_player),
        ("POST", "/api/admin/db/reset",      admin_db_reset),
        ("POST", "/api/admin/clock",         admin_clock),
        ("GET",  "/api/admin/analytics",     admin_analytics),
    ]
    for method, path, handler in routes:
        ui.expose_api(method, path, handler)
    logger.info("registered %d REST routes", len(routes))


# ── WebSocket ─────────────────────────────────────────────────────────────
#
# Rooms are joined by the client emitting the brick's built-in ``enter_room``
# event, so there is no server-side join handler to write here. The phone joins a
# room named for its token; the kiosk joins "kiosk".
#
# The countdown is rendered client-side from a deadline, so there is no
# per-second traffic. REST /api/answer remains the authoritative fallback if the
# socket drops, which is why the WebSocket answer path calls the same ApiService
# method rather than a separate one.

def _register_ws(ui: Any, api: ApiService) -> None:

    def on_connect(sid: str) -> None:
        logger.debug("socket connected: %s", sid)

    def on_disconnect(sid: str) -> None:
        logger.debug("socket disconnected: %s", sid)

    def on_message_rendered(sid: str, data: Any) -> dict[str, Any]:
        token = _field(data, "token")
        if not token:
            return {"ok": False, "err": "E_TOKEN"}
        status, payload = api.render_ack(token)
        return {"ok": status == 200, **payload}

    def on_answer_ws(sid: str, data: Any) -> dict[str, Any]:
        token = _field(data, "token")
        answer = _field(data, "answer")
        if not token:
            return {"ok": False, "err": "E_TOKEN"}
        # The IP is not available on a Socket.IO frame without reaching into the
        # brick's internals, so the WebSocket path passes a stable per-socket key.
        # That still buckets a retrying client correctly; it just cannot bucket by
        # network address. REST /api/answer remains the path that rate limits by
        # IP, which is why it is the authoritative fallback.
        status, payload = api.answer(token, answer, "phone", f"ws:{sid}")
        return {"status": status, **payload}

    ui.on_connect(on_connect)
    ui.on_disconnect(on_disconnect)
    ui.on_message("message_rendered", on_message_rendered)
    ui.on_message("answer_ws", on_answer_ws)
    logger.info("registered 4 WebSocket handlers")


def _field(data: Any, name: str) -> str:
    """Read a string field from a WebSocket payload, tolerating a missing key.

    The payload arrives from a browser, so it may be a dict, may lack the key, or
    may hold a non-string. Returning "" in every bad case lets the caller produce
    a clean error response instead of raising inside a Socket.IO callback, where
    an exception would be swallowed by the brick and leave the client waiting.
    """
    if not isinstance(data, dict):
        return ""
    value = data.get(name)
    return str(value) if value is not None else ""
