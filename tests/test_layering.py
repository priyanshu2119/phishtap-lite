"""
The layering rule, enforced by test.

docs/ARCHITECTURE.md §1 states that the game engine, DB and API handlers are pure
Python with no App Lab imports, and that only two thin adapters touch
``arduino.*``. That rule is what makes laptop-only development possible, and a
rule that is not checked is a rule that erodes: one convenient
``from arduino.app_utils import Bridge`` in the engine and the whole pytest suite
stops running off-board, silently, the day someone adds it.

So this test walks every source file and asserts the import graph.
"""

from __future__ import annotations

import ast
from pathlib import Path

import pytest

PACKAGE_ROOT = Path(__file__).resolve().parent.parent / "python" / "phishtap"

# The only two files permitted to import from the arduino namespace.
ALLOWED_APP_UTILS = "adapters/bridge_adapter.py"
ALLOWED_APP_BRICKS = "adapters/web_adapter.py"

FORBIDDEN_PREFIXES = ("arduino.app_utils", "arduino.app_bricks", "arduino.router_bridge")


def _all_modules() -> list[Path]:
    return sorted(p for p in PACKAGE_ROOT.rglob("*.py"))


def _arduino_imports(path: Path) -> list[str]:
    """Return every ``arduino.*`` module name imported by a source file.

    Walks the AST rather than grepping, so a mention inside a docstring or a
    comment does not produce a false failure. This file's own module docstring
    contains the string ``arduino.*`` and a grep-based check would flag it.
    """
    tree = ast.parse(path.read_text(encoding="utf-8"))
    found: list[str] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                if alias.name.startswith("arduino"):
                    found.append(alias.name)
        elif isinstance(node, ast.ImportFrom):
            if node.module and node.module.startswith("arduino"):
                found.append(node.module)
    return found


def test_package_root_exists():
    assert PACKAGE_ROOT.is_dir(), f"expected the package at {PACKAGE_ROOT}"


def test_every_module_is_scanned():
    """Guard against the test silently passing because it found no files."""
    modules = _all_modules()
    assert len(modules) >= 10, f"only found {len(modules)} modules, expected more"


@pytest.mark.parametrize("path", _all_modules(), ids=lambda p: str(p.relative_to(PACKAGE_ROOT)))
def test_arduino_imports_are_confined_to_the_adapters(path: Path):
    rel = str(path.relative_to(PACKAGE_ROOT)).replace("\\", "/")
    imports = _arduino_imports(path)

    if not imports:
        return

    for name in imports:
        if not name.startswith(FORBIDDEN_PREFIXES) and name != "arduino":
            continue
        if name.startswith("arduino.app_utils"):
            assert rel == ALLOWED_APP_UTILS, (
                f"{rel} imports {name}, but only {ALLOWED_APP_UTILS} may. "
                "Move the call behind the BridgePort interface instead."
            )
        elif name.startswith(("arduino.app_bricks", "arduino.router_bridge")):
            assert rel == ALLOWED_APP_BRICKS, (
                f"{rel} imports {name}, but only {ALLOWED_APP_BRICKS} may."
            )


def test_the_two_adapters_are_the_only_offenders():
    """Positive control: the adapters DO import arduino, so the scan works.

    Without this, the parametrized test above would also pass if ``_arduino_imports``
    were broken and always returned an empty list. A test that cannot fail is not
    a test, and this is the assertion that proves it can.
    """
    offenders = {
        str(p.relative_to(PACKAGE_ROOT)).replace("\\", "/")
        for p in _all_modules()
        if _arduino_imports(p)
    }
    assert offenders == {ALLOWED_APP_UTILS, ALLOWED_APP_BRICKS}, (
        f"expected exactly the two adapters to import arduino.*, got {sorted(offenders)}"
    )


def test_mock_bridge_does_not_import_the_real_adapter():
    """The mock must never drag in the real adapter.

    If it did, importing MockBridge on a laptop would import
    ``arduino.app_utils`` and the whole test suite would fail at collection. The
    real adapter's arduino import is lazy precisely so this stays true, and this
    test is what keeps it that way.
    """
    mock = PACKAGE_ROOT / "adapters" / "mock_bridge.py"
    source = mock.read_text(encoding="utf-8")
    assert "bridge_adapter" not in source, (
        "mock_bridge.py must not reference bridge_adapter; both implement "
        "BridgePort independently so the mock stays importable off-board"
    )


def test_engine_and_api_are_pure():
    """The two modules that hold all the logic must have no App Lab dependency."""
    for name in ("engine.py", "api.py", "sessions.py", "db.py", "scoring.py",
                 "corpus.py", "privacy.py", "ratelimit.py", "clock.py",
                 "bridge_port.py", "config.py"):
        path = PACKAGE_ROOT / name
        assert path.exists(), f"missing expected module {name}"
        assert not _arduino_imports(path), f"{name} must stay pure Python"


def test_corpus_is_not_web_served():
    """R6 in the architecture: the answers must not be publicly downloadable.

    The corpus lives at python/corpus.json and the web root is assets/. If a
    corpus file ever appears under assets/ the answers are served to anyone who
    asks, which defeats the entire purpose of the tool.
    """
    repo_root = Path(__file__).resolve().parent.parent
    assets = repo_root / "assets"
    if not assets.exists():
        pytest.skip("assets/ does not exist yet (Stage 3)")

    leaks = [
        p for p in assets.rglob("*")
        if p.is_file() and ("corpus" in p.name.lower() or p.suffix == ".json")
    ]
    # A JSON file under assets is not automatically a leak (a config file might
    # legitimately live there), so check the content for the answer field.
    for path in leaks:
        text = path.read_text(encoding="utf-8", errors="ignore")
        assert '"explanation"' not in text and '"label"' not in text, (
            f"{path} is under the web root and contains corpus answers"
        )
