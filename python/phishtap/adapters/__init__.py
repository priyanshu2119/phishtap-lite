"""
PhishTap Lite — adapters.

The only package in this project allowed to import ``arduino.*``. Everything
outside it is pure Python and runs identically on a laptop.

Two modules live here:

  * ``bridge_adapter`` — talks to the MCU over ``arduino.app_utils.Bridge``
  * ``mock_bridge``    — an in-process stand-in, for laptop development and tests

Both expose the same ``BridgePort`` protocol (defined in ``bridge_adapter``), so
the engine cannot tell which one it has. That is the point: swapping them is a
one-line change in ``main.py`` and nothing else needs to know.
"""
