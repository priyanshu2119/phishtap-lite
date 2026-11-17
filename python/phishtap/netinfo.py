"""
PhishTap Lite — resolving the URL a phone must actually open.

WHY THIS MODULE EXISTS
The WebUI brick builds its ``url`` property as
``f"{protocol}://{os.getenv('HOST_IP') or self._addr}:{port}"``. When App Lab
sets ``HOST_IP`` that is the board's reachable address and everything is fine.
When it is not set, ``self._addr`` is the bind address ``0.0.0.0``, and the
resulting URL is ``http://0.0.0.0:7000``.

That is a silent demo-killer. ``0.0.0.0`` is a valid *bind* address meaning "all
interfaces", but it is not a valid *destination*. The kiosk would render a QR code
encoding it, every phone in the room would fail to open it, and nothing in the
logs would look wrong because the server is genuinely listening and genuinely
healthy. The same applies to ``localhost`` and ``127.0.0.1``, which resolve to the
phone itself rather than the board.

So the base URL used for the QR code is validated here, and a wildcard or
loopback host is replaced with the board's actual LAN address.
"""

from __future__ import annotations

import logging
import socket
from urllib.parse import urlsplit, urlunsplit

logger = logging.getLogger(__name__)

# Hosts that are valid to bind to but meaningless to hand to a phone.
_NON_ROUTABLE_HOSTS: frozenset[str] = frozenset(
    {"0.0.0.0", "::", "", "localhost", "127.0.0.1", "::1"}
)


def lan_ip() -> str | None:
    """Best-effort LAN IPv4 address, without sending any traffic.

    Uses the UDP ``connect`` trick: connecting a UDP socket only selects the
    interface the kernel would route through, it does not transmit anything. That
    makes it work on an offline board with a laptop hotspot, which is the default
    demo network per docs/ARCHITECTURE.md §11 R14.

    Returns None if there is no default route at all, in which case the caller
    falls back to whatever the brick reported and logs loudly.
    """
    sock = None
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # The address is never contacted. A public DNS IP is used purely because
        # it is a well-known routable destination that makes the kernel pick the
        # default interface.
        sock.connect(("8.8.8.8", 80))
        ip = sock.getsockname()[0]
        return ip or None
    except OSError as e:
        logger.debug("could not determine the LAN address: %s", e)
        return None
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass


def is_phone_reachable(url: str) -> bool:
    """True if ``url`` has a host a phone could plausibly open."""
    try:
        host = urlsplit(url).hostname
    except ValueError:
        return False
    if host is None:
        return False
    return host.lower() not in _NON_ROUTABLE_HOSTS


def resolve_base_url(ui_url: str) -> tuple[str, str | None]:
    """Return ``(base_url, warning)``.

    ``base_url`` is a URL a phone on the same network can open. ``warning`` is a
    human-readable explanation when a substitution was made, or None when the
    brick's own URL was already usable.

    Substituting rather than failing is the right call: a wrong-but-logged URL
    still lets an operator read the address off the kiosk footer and type it in,
    whereas refusing to start would take the whole console down over something
    that has a workable answer.
    """
    if is_phone_reachable(ui_url):
        return ui_url, None

    parts = urlsplit(ui_url)
    ip = lan_ip()
    if ip is None:
        warning = (
            f"the web UI reported {ui_url!r}, which no phone can open, and no LAN "
            "address could be determined either. The kiosk QR code will not work "
            "until HOST_IP is set or the board has a default route."
        )
        logger.error(warning)
        return ui_url, warning

    fixed = urlunsplit((parts.scheme, f"{ip}:{parts.port}", parts.path,
                        parts.query, parts.fragment))
    warning = (
        f"the web UI reported {ui_url!r}, whose host is a bind address rather than "
        f"a destination. Using {fixed} instead, from the board's default interface. "
        "Set HOST_IP to override."
    )
    logger.warning(warning)
    return fixed, warning
