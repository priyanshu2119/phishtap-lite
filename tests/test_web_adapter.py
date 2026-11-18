"""
HTTP-level tests for the web adapter.

WHY THIS FILE EXISTS
Every POST endpoint was broken for the whole of Stage 2 and 351 tests passed.

The handlers were declared like this:

    def post_connect(request: Request, token: str = "") -> JSONResponse:

which reads as though it takes a JSON body. It does not. In FastAPI a SCALAR
parameter with a default is a QUERY parameter; only a Pydantic model or an
explicit Body(...) is read from the body. So a phone posting {"token": "abc"}
was silently ignored, `token` fell back to "", and the API replied "token is
required" to a request that had just supplied one. Scanning the QR code could
never work.

The reason no test caught it is that test_api.py calls ApiService methods
directly. That is the right way to test policy, and it is thorough — but it
never exercises the routing layer, which is where the bug lived. The only thing
that catches a request-parsing bug is issuing a request.

So these tests build a real FastAPI app, register the REAL handler functions
through the same call the brick uses, and issue real HTTP requests with
TestClient.

The fake UI below is not a simplification for convenience. Its expose_api is a
verbatim copy of the brick's, read out of the container image:

    self.app.add_api_route(self._api_path_prefix + path, function, methods=[method])

Matching it exactly is what makes these tests meaningful. A fake that registered
routes differently would test the fake.
"""

from __future__ import annotations

import pytest

fastapi = pytest.importorskip("fastapi", reason="the web adapter needs FastAPI")
TestClient = pytest.importorskip(
    "fastapi.testclient", reason="HTTP-level tests need FastAPI's TestClient (httpx)"
).TestClient

from phishtap.adapters.web_adapter import register_web  # noqa: E402

UID = "C7DA4104"


class FakeWebUI:
    """Stands in for arduino.app_bricks.web_ui.WebUI.

    expose_api mirrors the brick exactly. on_message/on_connect/on_disconnect and
    send_message are recorded so the WebSocket wiring can be asserted too, since
    the brick's Socket.IO manager cannot run on a laptop.
    """

    def __init__(self) -> None:
        self.app = fastapi.FastAPI()
        self._api_path_prefix = ""
        self.messages: list[tuple[str, object, str | None]] = []
        self.message_handlers: dict[str, object] = {}
        self.connect_cb = None
        self.disconnect_cb = None

    def expose_api(self, method: str, path: str, function) -> None:
        self.app.add_api_route(self._api_path_prefix + path, function, methods=[method])

    def on_connect(self, callback) -> None:
        self.connect_cb = callback

    def on_disconnect(self, callback) -> None:
        self.disconnect_cb = callback

    def on_message(self, message_type: str, callback) -> None:
        self.message_handlers[message_type] = callback

    def send_message(self, message_type: str, message, room: str | None = None) -> None:
        self.messages.append((message_type, message, room))


@pytest.fixture
def ui(api):
    fake = FakeWebUI()
    register_web(fake, api)
    return fake


@pytest.fixture
def client(ui):
    return TestClient(ui.app)


# ── The regression that this file exists for ──────────────────────────────

def test_post_connect_reads_the_json_body(client, engine, store, bridge):
    """THE bug. A JSON body must actually reach the handler.

    Before the fix this returned 400 E_TOKEN "token is required" for a request
    that had just supplied the token, which made QR-code joining impossible.
    """
    bridge.inject_tap(UID)
    engine.poll()
    session = store.active
    assert session is not None

    res = client.post("/api/session/connect", json={"token": session.token})
    assert res.status_code == 200, res.text
    body = res.json()
    assert body["ok"] is True, f"connect was rejected: {body}"
    assert body["token"] == session.token


def test_every_post_endpoint_accepts_a_json_body(client, engine, store, bridge):
    """All of them were broken the same way, so all of them are checked.

    A missing or empty field must produce the API's own precise E_* code, which
    proves the handler ran and read the body. A 422 from FastAPI would mean the
    framework rejected the request before the handler saw it, and a "required"
    error for a field that WAS sent means the body was ignored.
    """
    cases = [
        ("/api/session/connect", {"token": ""}, "E_TOKEN"),
        ("/api/join", {"pin": ""}, "E_PIN"),
        ("/api/consent", {"token": "", "nickname": "x", "consent_version": 1}, "E_TOKEN"),
        ("/api/render_ack", {"token": ""}, "E_TOKEN"),
        ("/api/answer", {"token": "", "answer": "phish"}, "E_TOKEN"),
        ("/api/delete_request", {"token": ""}, "E_TOKEN"),
        ("/api/admin/login", {"pin": ""}, "E_PIN"),
    ]
    for path, payload, expected_err in cases:
        res = client.post(path, json=payload)
        assert res.status_code != 422, (
            f"{path} returned 422, so FastAPI rejected the body before the "
            f"handler ran: {res.text}"
        )
        body = res.json()
        assert body.get("ok") is False, f"{path} unexpectedly succeeded: {body}"
        assert body.get("err") == expected_err, (
            f"{path} with {payload} gave err={body.get('err')!r}, expected "
            f"{expected_err!r}. A 'required' error for a field that was sent "
            f"means the JSON body was ignored."
        )


def test_an_empty_field_is_distinguishable_from_an_ignored_body(client):
    """Guard against the fix being "accept anything".

    Sending a real token must NOT produce E_TOKEN. If it did, the body would
    still be getting dropped and the test above would be passing for the wrong
    reason.
    """
    res = client.post("/api/session/connect", json={"token": "deadbeef" * 4})
    body = res.json()
    assert body.get("err") != "E_TOKEN", (
        "a supplied token was reported as missing, so the body is still ignored"
    )
    assert res.status_code == 404
    assert body.get("err") == "E_EXPIRED"


# ── Query-string fallback ─────────────────────────────────────────────────

def test_query_parameters_still_work_for_post(client, engine, store, bridge):
    """curl -X POST '.../api/session/connect?token=x' must keep working.

    Supporting both is deliberate: the browser sends a body, and whoever is
    debugging on the board at midnight with only curl should not have to work out
    the Content-Type incantation first.
    """
    bridge.inject_tap(UID)
    engine.poll()
    session = store.active

    res = client.post("/api/session/connect", params={"token": session.token})
    assert res.status_code == 200, res.text
    assert res.json()["ok"] is True


def test_the_body_wins_over_a_conflicting_query_parameter(client, engine, store, bridge):
    """Otherwise a stray query parameter could override what the client sent,
    which is a confusing way to fail and a mild injection surface."""
    bridge.inject_tap(UID)
    engine.poll()
    session = store.active

    res = client.post(
        "/api/session/connect",
        params={"token": "wrong" * 8},
        json={"token": session.token},
    )
    assert res.status_code == 200, res.text
    assert res.json()["token"] == session.token


# ── Malformed input must not 500 ──────────────────────────────────────────

def test_a_missing_body_yields_a_clean_error_not_a_500(client):
    res = client.post("/api/session/connect")
    assert res.status_code != 500, res.text
    body = res.json()
    assert body["ok"] is False
    assert body["err"] == "E_TOKEN"


def test_a_non_json_body_yields_a_clean_error_not_a_500(client):
    res = client.post(
        "/api/session/connect",
        content=b"this is not json",
        headers={"Content-Type": "application/json"},
    )
    assert res.status_code != 500, res.text
    assert res.json()["err"] == "E_TOKEN"


def test_a_json_array_body_yields_a_clean_error_not_a_500(client):
    """A JSON body that is valid but not an object. `out.update(body)` would
    raise on a list, so the handler must ignore it rather than crash."""
    res = client.post("/api/session/connect", json=["token", "abc"])
    assert res.status_code != 500, res.text
    assert res.json()["err"] == "E_TOKEN"


def test_a_null_field_is_treated_as_missing(client):
    """{"token": null} must give the same answer as omitting it, not a TypeError
    from deep inside the API layer."""
    res = client.post("/api/session/connect", json={"token": None})
    assert res.status_code != 500, res.text
    assert res.json()["err"] == "E_TOKEN"


def test_a_non_string_field_is_coerced_rather_than_crashing(client):
    res = client.post("/api/session/connect", json={"token": 12345})
    assert res.status_code != 500, res.text
    assert res.json()["ok"] is False


def test_a_non_numeric_consent_version_falls_back(client, engine, store, bridge):
    """consent_version is an int field. A string must not blow up the handler; it
    falls back to the default and the request is judged on its other fields."""
    bridge.inject_tap(UID)
    engine.poll()
    session = store.active
    client.post("/api/session/connect", json={"token": session.token})

    res = client.post("/api/consent", json={
        "token": session.token, "nickname": "Ananya", "consent_version": "not-a-number",
    })
    assert res.status_code != 500, res.text


# ── GET endpoints ─────────────────────────────────────────────────────────

def test_get_endpoints_use_query_parameters(client, engine, store, bridge):
    """GETs were never broken, but they are checked so a future refactor cannot
    break them while attention is on the POST side."""
    assert client.get("/api/health").json()["ok"] is True
    assert client.get("/api/kiosk").json()["ok"] is True
    assert client.get("/api/leaderboard").json()["ok"] is True

    bridge.inject_tap(UID)
    engine.poll()
    session = store.active
    res = client.get("/api/session", params={"token": session.token})
    assert res.status_code == 200, res.text
    assert res.json()["token"] == session.token


def test_get_session_without_a_token_is_a_400_not_a_500(client):
    res = client.get("/api/session")
    assert res.status_code == 400
    assert res.json()["err"] == "E_TOKEN"


# ── Admin: header auth plus a JSON body ───────────────────────────────────

def test_admin_login_reads_the_pin_from_the_body(client, api):
    pin = api.admin.get_or_create_pin()
    res = client.post("/api/admin/login", json={"pin": pin})
    assert res.status_code == 200, res.text
    body = res.json()
    assert body["ok"] is True
    assert body["admin_token"]


def test_admin_endpoints_take_the_token_in_a_header_and_the_payload_in_the_body(
    client, api
):
    """Both at once, which is the combination that is easiest to get wrong.

    The token must NOT be acceptable as a query parameter: a credential in a URL
    lands in access logs, browser history and Referer headers.
    """
    pin = api.admin.get_or_create_pin()
    token = client.post("/api/admin/login", json={"pin": pin}).json()["admin_token"]

    res = client.get("/api/admin/analytics", headers={"X-Admin-Token": token})
    assert res.status_code == 200, res.text
    body = res.json()
    assert body["ok"] is True
    for key in ("cohort", "by_cue", "reaction_ms", "funnel"):
        assert key in body, f"analytics payload missing {key}"

    res = client.post(
        "/api/admin/clock",
        headers={"X-Admin-Token": token},
        json={"offset_ms": 1500},
    )
    assert res.status_code == 200, res.text
    assert res.json()["clock_offset_ms"] == 1500


def test_admin_endpoints_reject_a_missing_token(client):
    res = client.get("/api/admin/analytics")
    assert res.status_code == 401
    assert res.json()["err"] == "E_PIN"


def test_admin_db_reset_needs_the_literal_confirmation(client, api):
    pin = api.admin.get_or_create_pin()
    token = client.post("/api/admin/login", json={"pin": pin}).json()["admin_token"]

    res = client.post(
        "/api/admin/db/reset", headers={"X-Admin-Token": token}, json={"confirm": "yes"}
    )
    assert res.status_code == 400, res.text

    res = client.post(
        "/api/admin/db/reset", headers={"X-Admin-Token": token}, json={"confirm": "RESET"}
    )
    assert res.status_code == 200, res.text
    assert res.json()["reset"] is True


def test_admin_export_returns_csv_with_a_download_header(client, api):
    pin = api.admin.get_or_create_pin()
    token = client.post("/api/admin/login", json={"pin": pin}).json()["admin_token"]

    res = client.get("/api/admin/export", headers={"X-Admin-Token": token})
    assert res.status_code == 200, res.text
    assert "csv" in res.headers.get("content-type", "")
    assert "attachment" in res.headers.get("content-disposition", "")


# ── A whole game over HTTP ────────────────────────────────────────────────

def test_a_complete_game_played_over_http(client, engine, store, bridge, real_corpus):
    """The flow a real phone performs, end to end, through the routing layer.

    This is the test that would have caught the bug immediately: it cannot pass
    unless POST bodies are actually read.
    """
    bridge.inject_tap(UID)
    engine.poll()
    session = store.active
    assert session is not None
    token = session.token

    # 1. The phone connects with the token from the QR code.
    res = client.post("/api/session/connect", json={"token": token})
    assert res.status_code == 200, res.text
    assert res.json()["needs_consent"] is True

    # 2. Consent, which arms the first round.
    res = client.post("/api/consent", json={
        "token": token, "nickname": "Ananya", "consent_version": 1,
    })
    assert res.status_code == 200, res.text
    assert res.json()["ok"] is True

    # 3. Play until the game ends, answering correctly each time.
    seen_rounds = 0
    for _ in range(40):
        s = client.get("/api/session", params={"token": token}).json()
        if s["state"] in ("game_over", "review"):
            break
        current = s.get("current")
        assert current is not None, f"no round armed in state {s['state']}"

        item = current["item"]
        # The answer must not be present, or the whole exercise is pointless.
        for forbidden in ("label", "cues", "explanation", "lesson"):
            assert forbidden not in item, f"{forbidden} leaked to the phone over HTTP"

        # Find the correct label from the server-side corpus, not the payload.
        correct = next(i for i in real_corpus if i.id == item["id"]).label
        answer = "phish" if correct == 1 else "safe"

        client.post("/api/render_ack", json={"token": token})
        res = client.post("/api/answer", json={
            "token": token, "answer": answer, "source": "phone",
        })
        assert res.status_code == 200, res.text
        body = res.json()
        assert body["ok"] is True
        assert body["correct"] is True
        seen_rounds += 1

    assert seen_rounds > 0, "no rounds were played"

    # 4. The kiosk saw the game too.
    kiosk = client.get("/api/kiosk").json()
    assert kiosk["session"]["score"] > 0
    assert kiosk["session"]["nickname"] == "Ananya"

    # 5. The leaderboard recorded the player.
    board = client.get("/api/leaderboard").json()["leaderboard"]
    assert any(row["nickname"] == "Ananya" for row in board)


def test_a_wrong_answer_over_http_costs_a_life(client, engine, store, bridge, real_corpus):
    bridge.inject_tap(UID)
    engine.poll()
    token = store.active.token
    client.post("/api/session/connect", json={"token": token})
    client.post("/api/consent", json={
        "token": token, "nickname": "Ravi", "consent_version": 1,
    })

    s = client.get("/api/session", params={"token": token}).json()
    item = s["current"]["item"]
    correct = next(i for i in real_corpus if i.id == item["id"]).label
    wrong = "safe" if correct == 1 else "phish"

    client.post("/api/render_ack", json={"token": token})
    res = client.post("/api/answer", json={"token": token, "answer": wrong})
    body = res.json()
    assert body["ok"] is True
    assert body["correct"] is False
    assert body["lives"] == 2, "a wrong answer must cost exactly one life"
    assert body["points"] == 0


def test_a_bad_answer_value_is_rejected_over_http(client, engine, store, bridge):
    bridge.inject_tap(UID)
    engine.poll()
    token = store.active.token
    client.post("/api/session/connect", json={"token": token})
    client.post("/api/consent", json={
        "token": token, "nickname": "Meera", "consent_version": 1,
    })

    res = client.post("/api/answer", json={"token": token, "answer": "maybe"})
    assert res.status_code == 400, res.text
    assert res.json()["ok"] is False


# ── WebSocket wiring ──────────────────────────────────────────────────────

def test_the_socket_handlers_are_registered(ui):
    """The brick's Socket.IO manager cannot run on a laptop, so the wiring is
    asserted instead: the right message types must be bound."""
    assert "message_rendered" in ui.message_handlers
    assert "answer_ws" in ui.message_handlers
    assert ui.connect_cb is not None
    assert ui.disconnect_cb is not None


def test_the_notifier_emits_to_the_kiosk_room(engine, store, bridge, ui):
    """The engine pushes through WebNotifier, which must target the "kiosk" room.
    A broadcast instead would send one player's round to every phone in the room."""
    from phishtap.adapters.web_adapter import WebNotifier

    engine._notify = WebNotifier(ui)
    bridge.inject_tap(UID)
    engine.poll()

    assert ui.messages, "nothing was emitted to the browser"
    rooms = {room for _, _, room in ui.messages}
    assert rooms == {"kiosk"}, f"expected only the kiosk room, got {rooms}"
    events = {name for name, _, _ in ui.messages}
    assert "session_started" in events
