"""
Base URL resolution.

These tests guard against a failure mode that is invisible in every other test:
a kiosk that renders a QR code encoding ``http://0.0.0.0:7000``. The server is
listening, the API responds, the game plays — and no phone in the room can open
the link. Nothing logs an error, because from the server's own point of view
nothing is wrong.
"""

from __future__ import annotations

import pytest

from phishtap import netinfo


@pytest.fixture(autouse=True)
def _clean_env(monkeypatch):
    """Clear PT_BASE_URL for every test in this module.

    Without this, a developer who happens to have PT_BASE_URL exported sees the
    substitution tests fail with the override's value and no clue why, because
    the override short-circuits before any of the logic under test runs. Tests
    that want an override set it explicitly.
    """
    monkeypatch.delenv("PT_BASE_URL", raising=False)


# ── is_phone_reachable ────────────────────────────────────────────────────

@pytest.mark.parametrize("url", [
    "http://0.0.0.0:7000",
    "https://0.0.0.0:7000",
    "http://localhost:7000",
    "http://127.0.0.1:7000",
    "http://[::]:7000",
    "http://[::1]:7000",
])
def test_bind_and_loopback_hosts_are_not_phone_reachable(url):
    """0.0.0.0 means "all interfaces" when binding and nothing when dialling.
    Loopback is worse in a specific way: a phone that opens http://127.0.0.1:7000
    connects to ITSELF, so it gets a connection-refused rather than a timeout and
    the student concludes the board is broken."""
    assert netinfo.is_phone_reachable(url) is False


@pytest.mark.parametrize("url", [
    "http://192.168.1.50:7000",
    "https://10.0.0.7:7000",
    "http://172.20.10.3:7000",
    "http://unoq.local:7000",
])
def test_real_hosts_are_phone_reachable(url):
    assert netinfo.is_phone_reachable(url) is True


def test_a_malformed_url_is_not_reachable_rather_than_raising():
    """A bad URL must produce a usable answer, not an exception at startup."""
    assert netinfo.is_phone_reachable("not a url at all") is False
    assert netinfo.is_phone_reachable("") is False


# ── resolve_base_url ──────────────────────────────────────────────────────

def test_an_already_valid_url_is_returned_unchanged():
    url, warning = netinfo.resolve_base_url("http://192.168.1.50:7000")
    assert url == "http://192.168.1.50:7000"
    assert warning is None, "no substitution means nothing to warn about"


def test_a_wildcard_host_is_replaced_with_the_lan_address(monkeypatch):
    monkeypatch.setattr(netinfo, "lan_ip", lambda: "192.168.43.17")
    url, warning = netinfo.resolve_base_url("http://0.0.0.0:7000")
    assert url == "http://192.168.43.17:7000"
    assert warning is not None
    assert "0.0.0.0" in warning, "the warning must name what was wrong"


def test_the_port_survives_substitution(monkeypatch):
    """A non-default port is easy to drop when rebuilding a URL, and dropping it
    sends every phone to port 80 where nothing is listening."""
    monkeypatch.setattr(netinfo, "lan_ip", lambda: "10.0.0.9")
    url, _ = netinfo.resolve_base_url("http://0.0.0.0:8080")
    assert url == "http://10.0.0.9:8080"


def test_the_scheme_survives_substitution(monkeypatch):
    """TLS must not be silently downgraded. A phone that loads the kiosk over
    http when the server only speaks https gets a protocol error, and mixed
    content rules would block the Socket.IO connection anyway."""
    monkeypatch.setattr(netinfo, "lan_ip", lambda: "10.0.0.9")
    url, _ = netinfo.resolve_base_url("https://0.0.0.0:7000")
    assert url.startswith("https://"), "https was downgraded to http"
    assert url == "https://10.0.0.9:7000"


def test_localhost_is_replaced_too(monkeypatch):
    """Loopback is the subtle case: it looks like a real host, and the server
    genuinely answers on it, so nothing else in the system complains."""
    monkeypatch.setattr(netinfo, "lan_ip", lambda: "192.168.1.20")
    url, warning = netinfo.resolve_base_url("http://localhost:7000")
    assert url == "http://192.168.1.20:7000"
    assert warning is not None


def test_no_lan_address_returns_the_original_with_a_warning(monkeypatch, caplog):
    """Substituting nothing is better than substituting a guess, but it must be
    loud: the operator needs to know the QR code will not work before a room full
    of students discovers it."""
    monkeypatch.setattr(netinfo, "lan_ip", lambda: None)
    url, warning = netinfo.resolve_base_url("http://0.0.0.0:7000")
    assert url == "http://0.0.0.0:7000"
    assert warning is not None
    assert "QR code will not work" in warning
    assert any(r.levelname == "ERROR" for r in caplog.records)


def test_lan_ip_returns_a_string_or_none_and_never_raises():
    """Whatever the network state, this must not take the app down at startup."""
    result = netinfo.lan_ip()
    assert result is None or isinstance(result, str)
    if result is not None:
        assert result not in netinfo._NON_ROUTABLE_HOSTS


def test_lan_ip_does_not_return_a_loopback_address():
    """If the only route is loopback, returning 127.0.0.1 would be worse than
    returning None, because None produces a loud warning while loopback produces
    a plausible-looking URL that silently fails on every phone."""
    ip = netinfo.lan_ip()
    if ip is not None:
        assert not ip.startswith("127.")


# ── Container-internal detection ──────────────────────────────────────────
#
# This is the more dangerous failure. A wildcard host is obviously wrong and
# someone notices. A Docker bridge address looks exactly like a real LAN
# address, gets encoded into the QR code, and produces phones that silently time
# out with nothing in any log to explain why.

@pytest.mark.parametrize("ip", ["172.17.0.2", "172.17.15.9", "169.254.3.4"])
def test_docker_bridge_and_link_local_are_flagged(ip):
    assert netinfo.is_container_internal(ip) is True


@pytest.mark.parametrize("ip", [
    "192.168.1.50",     # typical home router
    "192.168.43.17",    # Android hotspot
    "172.20.10.3",      # iPhone hotspot - legitimately in 172.16/12
    "10.0.0.9",         # typical college network
])
def test_real_lan_and_hotspot_addresses_are_not_flagged(ip):
    """172.16.0.0/12 must NOT be flagged wholesale. iPhone hotspots hand out
    172.20.10.x, and a laptop hotspot is the recommended default demo network
    (docs/ARCHITECTURE.md R14), so flagging the whole range would cry wolf on
    exactly the setup the demo depends on."""
    assert netinfo.is_container_internal(ip) is False


def test_a_non_ip_host_is_not_container_internal():
    """A hostname like unoq.local cannot be parsed as an address and must not be
    treated as container-internal."""
    assert netinfo.is_container_internal("unoq.local") is False
    assert netinfo.is_container_internal("nonsense") is False


def test_a_container_internal_substitution_warns_at_error_level(monkeypatch, caplog):
    """Verified on this board: a bare `docker run` of the app image reports
    172.17.0.2. Substituting that silently would produce a QR code no phone can
    open, so it must be loud and must name the fix."""
    monkeypatch.delenv("PT_BASE_URL", raising=False)
    monkeypatch.setattr(netinfo, "lan_ip", lambda: "172.17.0.2")
    url, warning = netinfo.resolve_base_url("http://0.0.0.0:7000")

    assert url == "http://172.17.0.2:7000"
    assert warning is not None
    assert "CONTAINER-INTERNAL" in warning
    assert "PT_BASE_URL" in warning, "the warning must name the fix"
    assert any(r.levelname == "ERROR" for r in caplog.records), (
        "a container-internal address must be an error, not a warning: the app "
        "still starts and still serves the kiosk, so nothing else will complain"
    )


def test_a_normal_lan_substitution_is_only_a_warning(monkeypatch, caplog):
    """Not every substitution is a crisis. A real LAN address is the expected
    fallback and should not be reported at error level, or the operator learns to
    ignore the one that matters."""
    monkeypatch.delenv("PT_BASE_URL", raising=False)
    monkeypatch.setattr(netinfo, "lan_ip", lambda: "192.168.1.50")
    url, warning = netinfo.resolve_base_url("http://0.0.0.0:7000")

    assert url == "http://192.168.1.50:7000"
    assert warning is not None
    assert "CONTAINER-INTERNAL" not in warning
    assert not any(r.levelname == "ERROR" for r in caplog.records)


# ── Explicit override ─────────────────────────────────────────────────────

def test_pt_base_url_overrides_everything(monkeypatch):
    """At a demo the operator can see the address on the laptop and the code
    cannot. An explicit override is the reliable escape hatch when detection
    picks the wrong interface."""
    monkeypatch.setenv("PT_BASE_URL", "http://192.168.1.99:7000")
    # Even a perfectly good brick URL must yield to the override.
    url, warning = netinfo.resolve_base_url("http://10.0.0.1:7000")
    assert url == "http://192.168.1.99:7000"
    assert warning is None, "an explicit override is not a problem to warn about"


def test_pt_base_url_overrides_a_container_internal_detection(monkeypatch, caplog):
    """The override exists precisely for the case where detection is wrong, so it
    must win without also emitting the container-internal error."""
    monkeypatch.setenv("PT_BASE_URL", "http://192.168.43.17:7000")
    monkeypatch.setattr(netinfo, "lan_ip", lambda: "172.17.0.2")
    url, warning = netinfo.resolve_base_url("http://0.0.0.0:7000")
    assert url == "http://192.168.43.17:7000"
    assert warning is None
    assert not any(r.levelname == "ERROR" for r in caplog.records)


def test_a_blank_pt_base_url_is_ignored(monkeypatch):
    """An empty environment variable is how an unset one often arrives, and
    treating it as an override would produce a base URL of ''."""
    monkeypatch.setenv("PT_BASE_URL", "   ")
    monkeypatch.setattr(netinfo, "lan_ip", lambda: "192.168.1.50")
    url, _ = netinfo.resolve_base_url("http://0.0.0.0:7000")
    assert url == "http://192.168.1.50:7000"


def test_a_valid_brick_url_needs_no_override(monkeypatch):
    """The normal path: App Lab sets HOST_IP, the brick reports a real address,
    and nothing is substituted or logged."""
    monkeypatch.delenv("PT_BASE_URL", raising=False)
    url, warning = netinfo.resolve_base_url("http://192.168.1.50:7000")
    assert url == "http://192.168.1.50:7000"
    assert warning is None
