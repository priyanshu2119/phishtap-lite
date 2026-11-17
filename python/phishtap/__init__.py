"""
PhishTap Lite — Python backend.

Layering rule (docs/ARCHITECTURE.md §1): the game engine, DB, scoring, sessions
and API handlers are PURE PYTHON with no App Lab imports. Only two thin adapters
touch ``arduino.*``:

  * ``adapters/bridge_adapter.py``  — the only file importing ``arduino.app_utils``
  * ``adapters/web_adapter.py``     — the only file importing ``arduino.app_bricks``

That separation is what makes laptop-only development possible: a mock bridge
plus a local web server serving the same assets, with no board attached.
``tests/test_layering.py`` enforces it, because a rule that is not checked is a
rule that erodes.
"""

__version__ = "0.2.0"
