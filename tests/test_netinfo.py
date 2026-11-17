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
