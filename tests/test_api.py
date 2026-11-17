"""
API layer: status codes, rate limits, admin auth and data deletion.

These tests call ``ApiService`` methods directly rather than going through HTTP.
That is deliberate: the service returns ``(status, payload)`` tuples precisely so
the policy can be tested without a web server, and the FastAPI wiring in
``adapters/web_adapter.py`` is then thin enough to review by eye.
"""

from __future__ import annotations

import pytest

from phishtap.api import AdminAuth
from phishtap.config import DB_RESET_CONFIRM, JOIN_LOCKOUT_THRESHOLD
from phishtap.corpus import LABEL_PHISH
from phishtap.db import db
from phishtap.sessions import SessionState

UID = "C7DA4104"
IP = "10.0.0.7"


def _tap_and_connect(api, engine, store, bridge, uid=UID):
    bridge.inject_tap(uid)
    engine.poll()
    return store.active


def _start(api, engine, store, bridge, nickname="Ananya", uid=UID):
    session = _tap_and_connect(api, engine, store, bridge, uid)
    api.connect(session.token, IP)
    api.consent(session.token, nickname, 1, IP)
    return session


def _correct_answer_for(engine, session, items):
    payload = engine.session_payload(session.token)
    item = next(i for i in items if i.id == payload["current"]["item"]["id"])
    return "phish" if item.label == LABEL_PHISH else "safe"


# ── Health and kiosk ──────────────────────────────────────────────────────

def test_health_reports_version_and_bridge_state(api):
    status, body = api.health()
    assert status == 200
    assert body["ok"] is True
    assert "version" in body
    assert "proto" in body
    assert "uptime_s" in body
    assert "bridge_ok" in body


def test_health_leaks_nothing_about_players(api):
    _, body = api.health()
    for forbidden in ("player", "uid", "nickname", "score", "token"):
        assert forbidden not in body, f"health exposed {forbidden}"


def test_kiosk_includes_the_session_url(api, engine, store, bridge):
    session = _tap_and_connect(api, engine, store, bridge)
    status, body = api.kiosk()
    assert status == 200
    assert body["session"]["token"] == session.token
    assert body["session"]["pin"] == session.pin
    url = body["session"]["url"]
    assert url.startswith("http://192.168.1.50:7000/game.html?token=")
    assert session.token in url


def test_kiosk_url_uses_the_configured_base(api):
    """DHCP can change the board's address, so the QR must be built from the
    runtime base URL rather than a hardcoded one."""
    api.base_url = "http://192.168.43.10:7000"
    _, body = api.kiosk()
    assert body["base_url"] == "http://192.168.43.10:7000"


def test_kiosk_includes_the_leaderboard(api):
    _, body = api.kiosk()
    assert "leaderboard" in body
    assert isinstance(body["leaderboard"], list)


def test_kiosk_reports_link_health(api):
    _, body = api.kiosk()
    assert "link_ok" in body
    assert "mcu" in body


# ── Session lookup ────────────────────────────────────────────────────────

def test_get_session_returns_the_current_round(api, engine, store, bridge, real_corpus):
    session = _start(api, engine, store, bridge)
    status, body = api.get_session(session.token)
    assert status == 200
    assert body["state"] == "awaiting_answer"
    assert "current" in body
    assert "label" not in body["current"]["item"]


def test_get_session_with_an_unknown_token_is_404(api):
    status, body = api.get_session("f" * 32)
    assert status == 404
    assert body["err"] == "E_EXPIRED"


def test_get_session_without_a_token_is_400(api):
    status, body = api.get_session("")
    assert status == 400
    assert body["err"] == "E_TOKEN"


# ── Connect and rate limiting ─────────────────────────────────────────────

def test_connect_returns_the_join_payload(api, engine, store, bridge):
    session = _tap_and_connect(api, engine, store, bridge)
    status, body = api.connect(session.token, IP)
    assert status == 200
    assert body["needs_consent"] is True


def test_connect_is_rate_limited_per_ip(api, engine, store, bridge):
    from phishtap.config import RATE_CONNECT_PER_MIN

    session = _tap_and_connect(api, engine, store, bridge)
    codes = [api.connect(session.token, IP)[0] for _ in range(RATE_CONNECT_PER_MIN + 5)]
    assert 429 in codes, "connect was never rate limited"
    assert codes.count(429) >= 1


def test_rate_limits_are_per_ip_not_global(api, engine, store, bridge):
    """One student hammering reconnect must not lock out the rest of the room."""
    from phishtap.config import RATE_CONNECT_PER_MIN

    session = _tap_and_connect(api, engine, store, bridge)
    for _ in range(RATE_CONNECT_PER_MIN + 3):
        api.connect(session.token, "10.0.0.99")
    status, _ = api.connect(session.token, "10.0.0.100")
    assert status == 200


def test_connect_with_an_unknown_token_is_404(api):
    status, body = api.connect("f" * 32, IP)
    assert status == 404
    assert body["err"] == "E_EXPIRED"


# ── PIN join ──────────────────────────────────────────────────────────────

def test_join_with_the_correct_pin(api, engine, store, bridge):
    session = _tap_and_connect(api, engine, store, bridge)
    status, body = api.join(session.pin, IP)
    assert status == 200
    assert body["token"] == session.token


def test_join_with_a_wrong_pin_is_404(api, engine, store, bridge):
    session = _tap_and_connect(api, engine, store, bridge)
    wrong = "0000" if session.pin != "0000" else "0001"
    status, body = api.join(wrong, IP)
    assert status == 404
    assert body["err"] == "E_PIN"
    assert "attempt" in body["message"]


def test_join_locks_out_after_repeated_failures(api, engine, store, bridge):
    """A 4-digit PIN is only 10,000 possibilities, so anyone on the same Wi-Fi
    could brute force it without a lockout."""
    session = _tap_and_connect(api, engine, store, bridge)
    wrong = "0000" if session.pin != "0000" else "0001"
    for _ in range(JOIN_LOCKOUT_THRESHOLD):
        api.join(wrong, IP)
    status, body = api.join(session.pin, IP)
    assert status == 423, "the correct PIN should be refused once locked out"
    assert body["err"] == "E_PIN"


def test_a_correct_pin_clears_the_failure_count(api, engine, store, bridge):
    session = _tap_and_connect(api, engine, store, bridge)
    wrong = "0000" if session.pin != "0000" else "0001"
    for _ in range(JOIN_LOCKOUT_THRESHOLD - 1):
        api.join(wrong, IP)
    assert api.join(session.pin, IP)[0] == 200
    # Failures below the threshold again, so the counter was cleared.
    for _ in range(JOIN_LOCKOUT_THRESHOLD - 1):
        status, _ = api.join(wrong, IP)
        assert status == 404, "counter was not reset by the successful join"


def test_join_lockout_is_per_ip(api, engine, store, bridge):
    session = _tap_and_connect(api, engine, store, bridge)
    wrong = "0000" if session.pin != "0000" else "0001"
    for _ in range(JOIN_LOCKOUT_THRESHOLD + 2):
        api.join(wrong, "10.0.0.50")
    assert api.join(session.pin, "10.0.0.51")[0] == 200


# ── Consent ───────────────────────────────────────────────────────────────

def test_consent_starts_the_game(api, engine, store, bridge):
    session = _tap_and_connect(api, engine, store, bridge)
    api.connect(session.token, IP)
    status, body = api.consent(session.token, "Ananya", 1, IP)
    assert status == 200
    assert body["ok"] is True
    assert body["nickname"] == "Ananya"
    assert len(body["player_id"]) == 16


def test_consent_rejects_a_short_nickname(api, engine, store, bridge):
    session = _tap_and_connect(api, engine, store, bridge)
    api.connect(session.token, IP)
    status, body = api.consent(session.token, "ab", 1, IP)
    assert status == 400
    assert body["err"] == "E_NICKNAME"


def test_consent_rejects_a_blocked_nickname(api, engine, store, bridge):
    session = _tap_and_connect(api, engine, store, bridge)
    api.connect(session.token, IP)
    status, body = api.consent(session.token, "fuckhead", 1, IP)
    assert status == 400
    assert body["err"] == "E_NICKNAME"


def test_consent_with_an_unknown_token_is_404(api):
    status, body = api.consent("f" * 32, "Ananya", 1, IP)
    assert status == 404
    assert body["err"] == "E_EXPIRED"


def test_consent_twice_is_409(api, engine, store, bridge):
    session = _start(api, engine, store, bridge)
    status, body = api.consent(session.token, "Someone", 1, IP)
    assert status == 409
    assert body["err"] == "E_CONSENT"


# ── render_ack and answer ─────────────────────────────────────────────────

def test_render_ack_is_always_200(api, engine, store, bridge):
    session = _start(api, engine, store, bridge)
    assert api.render_ack(session.token)[0] == 200


def test_render_ack_for_an_unknown_token_is_still_200(api):
    """A late ack from a phone that has already moved on is normal, not an error.
    Failing it would make the client retry into a loop."""
    assert api.render_ack("f" * 32)[0] == 200


def test_answer_correct_scores(api, engine, store, bridge, real_corpus):
    session = _start(api, engine, store, bridge)
    answer = _correct_answer_for(engine, session, real_corpus)
    status, body = api.answer(session.token, answer, "phone", IP)
    assert status == 200
    assert body["correct"] is True
    assert body["points"] >= 10


def test_answer_rejects_an_invalid_value(api, engine, store, bridge):
    session = _start(api, engine, store, bridge)
    status, body = api.answer(session.token, "maybe", "phone", IP)
    assert status == 400


def test_answer_without_a_token_is_400(api):
    status, body = api.answer("", "phish", "phone", IP)
    assert status == 400
    assert body["err"] == "E_TOKEN"


def test_answer_with_an_unknown_token_is_404(api):
    status, body = api.answer("f" * 32, "phish", "phone", IP)
    assert status == 404


def test_answer_records_the_source(api, engine, store, bridge, real_corpus):
    """Phone and button timings must never be mixed in one distribution, so the
    source has to be stored on every row."""
    session = _start(api, engine, store, bridge)
    answer = _correct_answer_for(engine, session, real_corpus)
    api.answer(session.token, answer, "button", IP)
    row = db().execute(
        "SELECT answer_source FROM answers WHERE session_token=?", (session.token,)
    ).fetchone()
    assert row["answer_source"] == "button"


def test_answer_after_the_round_closed_is_409_not_400(api, engine, store, bridge, real_corpus):
    """Regression guard for the exception-ordering bug.

    A round that has already been graded must produce 409 (conflict, stop and
    refresh) and not 400 (your input was malformed, fix it and retry). Returning
    400 makes a client retry forever, because from its point of view nothing about
    the request was wrong.
    """
    session = _start(api, engine, store, bridge)
    answer = _correct_answer_for(engine, session, real_corpus)
    assert api.answer(session.token, answer, "phone", IP)[0] == 200

    # Force the session out of AWAITING_ANSWER to simulate a round that closed
    # while this answer was in flight.
    session.state = SessionState.GAME_OVER
    status, body = api.answer(session.token, answer, "phone", IP)
    assert status == 409, f"expected 409, got {status} with {body}"
    assert body["err"] == "E_EXPIRED"


# ── Review and deletion ───────────────────────────────────────────────────

def _finish_game(api, engine, store, bridge, real_corpus, session):
    for _ in range(3):
        if session.state.value == "game_over":
            break
        answer = _correct_answer_for(engine, session, real_corpus)
        wrong = "safe" if answer == "phish" else "phish"
        api.answer(session.token, wrong, "phone", IP)
    return session.review_token


def test_review_reveals_the_answers(api, engine, store, bridge, real_corpus):
    session = _start(api, engine, store, bridge)
    review_token = _finish_game(api, engine, store, bridge, real_corpus, session)
    status, body = api.review(review_token)
    assert status == 200
    assert body["rounds_played"] == 3
    for r in body["rounds"]:
        assert "correct_label" in r
        assert r["explanation"]
        assert r["lesson"]


def test_review_with_an_unknown_token_is_404(api):
    status, body = api.review("f" * 32)
    assert status == 404
    assert body["err"] == "E_EXPIRED"


def test_delete_request_removes_the_players_data(api, engine, store, bridge, real_corpus):
    session = _start(api, engine, store, bridge)
    player_id = session.player_id
    review_token = _finish_game(api, engine, store, bridge, real_corpus, session)

    status, body = api.delete_request(review_token)
    assert status == 200
    assert body["deleted"] == player_id

    conn = db()
    assert conn.execute(
        "SELECT COUNT(*) c FROM answers WHERE player_id=?", (player_id,)
    ).fetchone()["c"] == 0
    assert conn.execute(
        "SELECT COUNT(*) c FROM sessions WHERE player_id=?", (player_id,)
    ).fetchone()["c"] == 0
    row = conn.execute(
        "SELECT deleted, nickname FROM players WHERE player_id=?", (player_id,)
    ).fetchone()
    assert row["deleted"] == 1
    assert row["nickname"].startswith("[deleted-")


def test_delete_request_is_scoped_to_the_review_token(api, engine, store, bridge, real_corpus):
    """Deletion must not be reachable by a card tap: in shared-card mode a tap
    cannot say WHO is asking, so it would let one student erase another's data."""
    session = _start(api, engine, store, bridge)
    _finish_game(api, engine, store, bridge, real_corpus, session)
    status, _ = api.delete_request("f" * 32)
    assert status == 404
    row = db().execute(
        "SELECT deleted FROM players WHERE player_id=?", (session.player_id,)
    ).fetchone()
    assert row["deleted"] == 0


def test_deleted_players_leave_the_leaderboard(api, engine, store, bridge, real_corpus):
    session = _start(api, engine, store, bridge)
    _finish_game(api, engine, store, bridge, real_corpus, session)
    api.delete_request(session.review_token)
    _, body = api.leaderboard()
    assert all(e["nickname"] != "Ananya" for e in body["leaderboard"])


# ── Admin ─────────────────────────────────────────────────────────────────

def test_admin_login_with_the_correct_pin(api):
    pin = api.admin.get_or_create_pin()
    status, body = api.admin_login(pin, IP)
    assert status == 200
    assert len(body["admin_token"]) == 32


def test_admin_login_with_a_wrong_pin_is_401(api):
    status, body = api.admin_login("000000" if api.admin.get_or_create_pin() != "000000" else "000001", IP)
    assert status == 401
    assert body["err"] == "E_PIN"


def test_admin_endpoints_require_a_token(api):
    assert api.admin_export(None)[0] == 401
    assert api.admin_delete_player(None, "x")[0] == 401
    assert api.admin_db_reset(None, DB_RESET_CONFIRM)[0] == 401
    assert api.admin_clock(None, 0)[0] == 401


def test_admin_endpoints_reject_a_bogus_token(api):
    bogus = "f" * 32
    assert api.admin_export(bogus)[0] == 401
    assert api.admin_delete_player(bogus, "x")[0] == 401
    assert api.admin_db_reset(bogus, DB_RESET_CONFIRM)[0] == 401
    assert api.admin_clock(bogus, 0)[0] == 401


def test_admin_export_returns_csv(api, engine, store, bridge, real_corpus):
    session = _start(api, engine, store, bridge)
    answer = _correct_answer_for(engine, session, real_corpus)
    api.answer(session.token, answer, "phone", IP)

    pin = api.admin.get_or_create_pin()
    token = api.admin_login(pin, IP)[1]["admin_token"]
    status, content_type, body = api.admin_export(token)
    assert status == 200
    assert content_type.startswith("text/csv")
    lines = body.strip().splitlines()
    assert lines[0].startswith("player_id,session_token,round_index")
    assert len(lines) == 2


def test_admin_export_contains_no_nicknames_or_uids(api, engine, store, bridge, real_corpus):
    """The export is the R7 backup, and it leaves the board, so it must carry only
    the pseudonymous player_id and the measurements."""
    session = _start(api, engine, store, bridge)
    answer = _correct_answer_for(engine, session, real_corpus)
    api.answer(session.token, answer, "phone", IP)

    token = api.admin_login(api.admin.get_or_create_pin(), IP)[1]["admin_token"]
    _, _, body = api.admin_export(token)
    assert "Ananya" not in body
    assert UID not in body


def test_db_reset_requires_the_literal_confirmation(api, engine, store, bridge, real_corpus):
    _start(api, engine, store, bridge)
    token = api.admin_login(api.admin.get_or_create_pin(), IP)[1]["admin_token"]
    status, body = api.admin_db_reset(token, "yes")
    assert status == 400
    assert db().execute("SELECT COUNT(*) c FROM sessions").fetchone()["c"] == 1


def test_db_reset_clears_everything(api, engine, store, bridge, real_corpus):
    _start(api, engine, store, bridge)
    token = api.admin_login(api.admin.get_or_create_pin(), IP)[1]["admin_token"]
    status, body = api.admin_db_reset(token, DB_RESET_CONFIRM)
    assert status == 200
    conn = db()
    for table in ("players", "sessions", "answers", "tap_log"):
        assert conn.execute(f"SELECT COUNT(*) c FROM {table}").fetchone()["c"] == 0  # noqa: S608


def test_clock_offset_is_persisted(api):
    token = api.admin_login(api.admin.get_or_create_pin(), IP)[1]["admin_token"]
    status, body = api.admin_clock(token, 123456)
    assert status == 200
    assert body["clock_offset_ms"] == 123456
    row = db().execute(
        "SELECT value FROM meta WHERE key='clock_offset_ms'"
    ).fetchone()
    assert row["value"] == "123456"
    assert api.clock.offset_ms == 123456


def test_admin_login_is_rate_limited(api):
    from phishtap.config import RATE_ADMIN_LOGIN_PER_MIN

    codes = [api.admin_login("000000", IP)[0] for _ in range(RATE_ADMIN_LOGIN_PER_MIN + 5)]
    assert 429 in codes


# ── AdminAuth ─────────────────────────────────────────────────────────────

def test_admin_pin_is_generated_once(tmp_path):
    path = tmp_path / "admin_pin"
    first = AdminAuth(path).get_or_create_pin()
    second = AdminAuth(path).get_or_create_pin()
    assert first == second
    assert len(first) == 6
    assert first.isdigit()


def test_admin_session_tokens_expire(tmp_path, monkeypatch):
    from phishtap import api as api_mod

    auth = AdminAuth(tmp_path / "pin")
    pin = auth.get_or_create_pin()
    token = auth.verify(pin)
    assert auth.check(token) is True

    monkeypatch.setattr(api_mod, "monotonic_ms", lambda: 10**15)
    assert auth.check(token) is False, "an admin session must not live forever"


def test_admin_token_can_be_revoked(tmp_path):
    auth = AdminAuth(tmp_path / "pin")
    token = auth.verify(auth.get_or_create_pin())
    auth.revoke(token)
    assert auth.check(token) is False


def test_admin_pin_file_is_mode_0600(tmp_path):
    path = tmp_path / "admin_pin"
    AdminAuth(path).get_or_create_pin()
    assert path.stat().st_mode & 0o777 == 0o600
