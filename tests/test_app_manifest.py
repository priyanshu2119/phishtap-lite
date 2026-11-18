"""
App Lab manifest validation.

``app.yaml`` is the only file that decides whether App Lab treats this folder as
an app at all, and it is edited by hand. Two mistakes are easy to make and both
fail at the worst possible moment — when someone presses Run in front of an
audience:

  * A missing ``app.yaml`` means the app never appears in the Apps list, while the
    directory still reserves the name, so "Create new app" rejects it as already
    in use and there is no way out from the browser.
  * A port declared both here and by a brick makes App Lab refuse to start:
    ``port 7000 is declared by multiple sources: arduino:web_ui, app.yaml``.

The second one happened for real. These tests exist so it cannot happen again.
"""

from __future__ import annotations

from pathlib import Path

import pytest

yaml = pytest.importorskip(
    "yaml",
    reason="PyYAML is needed to parse app.yaml; it ships in the App Lab container "
           "image and is a pytest-time-only dependency, never imported at runtime",
)

REPO_ROOT = Path(__file__).resolve().parent.parent
APP_YAML = REPO_ROOT / "app.yaml"

# Ports each brick declares for itself, transcribed from the board's brick
# registry at /var/lib/arduino-app-cli/assets/<version>/bricks-list.yaml:
#
#     - id: arduino:web_ui
#       ports:
#       - 7000
#
# Hardcoded rather than read from the board, because the tests must run on a
# laptop with no board attached. The trade is that this table can go stale if
# Arduino changes a brick's port; test_brick_port_table_matches_the_board
# documents how to re-verify it.
BRICK_PORTS: dict[str, tuple[int, ...]] = {
    "arduino:web_ui": (7000,),
}


@pytest.fixture(scope="module")
def manifest() -> dict:
    assert APP_YAML.exists(), (
        f"{APP_YAML} is missing. Without it App Lab does not list the app, but the "
        "directory still reserves the name, so 'Create new app' rejects it as "
        "already in use and there is no way out from the browser."
    )
    data = yaml.safe_load(APP_YAML.read_text(encoding="utf-8"))
    assert isinstance(data, dict), "app.yaml must be a mapping"
    return data


def test_manifest_declares_a_name_matching_the_deploy_target(manifest):
    """The name must match the folder under /home/arduino/ArduinoApps/, which is
    what tools/deploy_to_board.sh pushes to."""
    assert manifest.get("name") == "phishtap"


def test_manifest_has_an_icon(manifest):
    """App Lab renders this in the Apps list. Missing it is not fatal but the app
    shows a blank tile, which looks broken."""
    assert manifest.get("icon"), "app.yaml should declare an icon"


def test_manifest_declares_the_web_ui_brick(manifest):
    """Without the brick there is no FastAPI, no Socket.IO and no static file
    serving, so the kiosk and the phone game have nothing to talk to."""
    bricks = manifest.get("bricks") or []
    assert "arduino:web_ui" in bricks, (
        "app.yaml must declare the arduino:web_ui brick, or no web server starts"
    )


def test_no_port_is_declared_by_both_the_app_and_a_brick(manifest):
    """THE REGRESSION GUARD.

    App Lab refuses to start when a port comes from two sources:

        port 7000 is declared by multiple sources: arduino:web_ui, app.yaml

    The brick owns its port. app.yaml declares only ports that no brick provides.
    """
    declared = manifest.get("ports") or []
    bricks = manifest.get("bricks") or []

    brick_owned: set[int] = set()
    for brick in bricks:
        brick_owned.update(BRICK_PORTS.get(brick, ()))

    overlap = sorted(set(declared) & brick_owned)
    assert not overlap, (
        f"port(s) {overlap} are declared both in app.yaml and by a brick "
        f"({', '.join(bricks)}). App Lab will refuse to start with "
        "'port ... is declared by multiple sources'. Remove them from app.yaml; "
        "the brick already publishes them."
    )


def test_ports_is_an_explicit_empty_list_not_a_missing_key(manifest):
    """``ports: []`` rather than omitting the key.

    App Lab's own generated manifests always include the key, and an absent key
    is ambiguous between "no ports" and "not yet decided". Being explicit also
    means the comment above it stays attached to something.
    """
    assert "ports" in manifest, "app.yaml should declare ports explicitly, even if empty"
    assert manifest["ports"] == [], (
        "no port should be listed while arduino:web_ui provides 7000; see "
        "test_no_port_is_declared_by_both_the_app_and_a_brick"
    )


def test_the_web_port_is_still_published_by_someone(manifest):
    """Guard against "fixing" the duplicate by deleting the brick instead.

    Port 7000 must be reachable from a phone, and after the duplicate declaration
    was removed the only thing publishing it is the brick. If the brick were also
    removed, nothing would serve the kiosk and the failure would be silent.
    """
    bricks = manifest.get("bricks") or []
    declared = set(manifest.get("ports") or [])

    published_by_brick = any(7000 in BRICK_PORTS.get(b, ()) for b in bricks)
    assert published_by_brick or 7000 in declared, (
        "nothing publishes port 7000. Either declare the arduino:web_ui brick "
        "(preferred, it owns the port) or list 7000 under ports."
    )


def test_brick_port_table_matches_the_board():
    """Document how to re-verify BRICK_PORTS against a real board.

    This is not an assertion that can run on a laptop — it is the instruction for
    whoever suspects the table has gone stale after an App Lab update. Kept as a
    test so it is discoverable next to the table it validates.
    """
    instructions = (
        "adb shell 'grep -A6 \"id: arduino:web_ui\" "
        "/var/lib/arduino-app-cli/assets/*/bricks-list.yaml'"
    )
    assert instructions.startswith("adb shell")
    assert BRICK_PORTS["arduino:web_ui"] == (7000,)


def test_manifest_is_valid_yaml_with_no_tabs(manifest):
    """YAML forbids tabs for indentation. A hand-edited manifest that mixes them
    parses differently than it looks, or fails outright."""
    raw = APP_YAML.read_text(encoding="utf-8")
    for lineno, line in enumerate(raw.splitlines(), start=1):
        assert "\t" not in line, f"app.yaml line {lineno} contains a tab"
    # Re-serialising proves the parse was lossless enough to be meaningful.
    assert yaml.safe_load(yaml.safe_dump(manifest)) == manifest


# ── Board dependencies ────────────────────────────────────────────────────

def test_the_board_has_no_requirements_file():
    """python/requirements.txt must not exist.

    App Lab's container entrypoint installs it at every start, and it counts
    COMMENT lines as non-blank:

        REQUIREMENTS_LINES="$(grep -c '[^[:space:]]' "$REQUIREMENTS_FILE")"

    so even an all-comment file triggers `uv pip install` and needs network access
    at startup. A guessed pin in that file previously made resolution fail:

        error: No solution found when resolving dependencies
          cause: Because only fastapi-socketio<=0.0.10 is available and you
                 require fastapi-socketio>=1.0.2 ...

    The app still started because the brick's own dependencies were already
    present, but the failure cleared the uv cache and set DEPS_FAILED, which makes
    the prepare phase fail. Laptop-only dependencies belong in requirements-dev.txt
    at the repository root, outside python/, where the entrypoint never looks.
    """
    board_requirements = REPO_ROOT / "python" / "requirements.txt"
    assert not board_requirements.exists(), (
        "python/requirements.txt is installed by the App Lab container at every "
        "start and needs network access. This backend imports only `arduino` and "
        "`fastapi`, both already in the image. Put laptop-only dependencies in "
        "requirements-dev.txt at the repository root instead."
    )


def test_laptop_dependencies_live_outside_the_deployed_tree():
    """The dev requirements file must exist and must not be under python/."""
    dev = REPO_ROOT / "requirements-dev.txt"
    assert dev.exists(), "requirements-dev.txt is missing; laptop tests need pytest"
    assert dev.parent == REPO_ROOT, (
        "requirements-dev.txt must sit at the repository root, not under python/, "
        "or the App Lab entrypoint will try to install it on the board"
    )
    text = dev.read_text(encoding="utf-8").lower()
    assert "pytest" in text, "pytest must be declared for the laptop suite"


def test_the_backend_imports_nothing_the_image_lacks():
    """Every third-party import in python/ must be provided by the container image.

    The image ships `arduino` (app-bricks-py) and `fastapi` (via the web_ui
    brick). Anything else would need a requirements file, which the test above
    forbids, so this is the check that keeps those two rules consistent.

    AST-parsed rather than imported, because importing phishtap.adapters would
    pull in arduino.* and fail on a laptop.
    """
    import ast
    import sys

    allowed = {"arduino", "fastapi"}
    stdlib = set(sys.stdlib_module_names)
    package_root = REPO_ROOT / "python" / "phishtap"

    offenders: dict[str, set[str]] = {}
    for path in sorted(package_root.rglob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            names: list[str] = []
            if isinstance(node, ast.Import):
                names = [a.name.split(".")[0] for a in node.names]
            elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
                names = [node.module.split(".")[0]]
            for name in names:
                if name in stdlib or name == "phishtap":
                    continue
                if name not in allowed:
                    offenders.setdefault(name, set()).add(path.name)

    assert not offenders, (
        f"third-party imports not provided by the container image: {offenders}. "
        "The board has no requirements.txt by design, so either the package is "
        "already in the image or it cannot be used."
    )
