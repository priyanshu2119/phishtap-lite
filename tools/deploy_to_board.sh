#!/usr/bin/env bash
#
# PhishTap Lite — push the multi-file sketch to the UNO Q over ADB
#
# WHY ADB AND NOT A SINGLE MERGED FILE
#   Merging the headers into one .ino looks convenient but CANNOT build. See the
#   ".ino PURITY RULE" section at the top of tools/syntax_check.sh: arduino-cli
#   hoists a prototype for every function defined in the .ino, above the typedefs
#   and above the later #include lines, so helpers whose signatures use
#   PtVerdict / PtBuzzPattern / MFRC522::StatusCode fail with errors that do not
#   correspond to anything visibly wrong in the source. Keeping the helpers in
#   headers avoids it completely, because arduino-cli only processes the .ino.
#
#   ADB over USB-C needs no network at all, so this also works when the Wi-Fi is
#   being difficult.
#
# USAGE
#   tools/deploy_to_board.sh                          deploy hw_selftest, phases unchanged
#   tools/deploy_to_board.sh --sketch phishtap        deploy the production firmware
#   tools/deploy_to_board.sh --phases=A               deploy with only Phase A enabled
#   tools/deploy_to_board.sh --phases=A,B
#   tools/deploy_to_board.sh --phases=L               SPI loopback, RC522 REMOVED
#   tools/deploy_to_board.sh --phases=W               MISO line state, RC522 wired
#   tools/deploy_to_board.sh --phases=all
#   tools/deploy_to_board.sh --app myapp              target a different app folder
#   tools/deploy_to_board.sh --check-lib              also pre-flight the pinned libraries
#   tools/deploy_to_board.sh --loop                   repeat the selftest forever
#   tools/deploy_to_board.sh --dry-run                show what would happen
#
#   --sketch selects which folder under sketch/ to push, and defaults to
#   hw_selftest. The app folder on the board takes the same name unless --app
#   overrides it. --phases and --loop are hw_selftest-only: they rewrite that
#   sketch's RUN_PHASE_* and PT_LOOP_FOREVER guards, which the production
#   firmware does not have, so combining them with another --sketch is rejected
#   rather than silently doing nothing.
#
#   Phase letters: A board only, L SPI loopback (reader removed, D11-D12 jumper),
#   W MISO line state (reader wired), B full RC522 test, C1/C2/C3 LEDs and buzzer.
#   tools/run_phase.sh is a friendlier wrapper around the common combinations.
#
#   --loop is worth using when diagnosing: the selftest normally prints everything
#   from setup() in the first seconds, so if App Lab's Serial monitor tab is not
#   already open the output is missed entirely. Looping means a full pass will
#   come round again however late the monitor is opened.
#
# EXIT STATUS
#   0 success, 1 push failed, 2 usage / adb missing

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKETCH="hw_selftest"
APP=""
PHASES=""
DRY_RUN=0
CHECK_LIB=0
LOOP_FOREVER=0

while [ $# -gt 0 ]; do
  case "$1" in
    --sketch)   SKETCH="${2:?--sketch needs a name}"; shift 2 ;;
    --sketch=*) SKETCH="${1#--sketch=}"; shift ;;
    --app)     APP="${2:?--app needs a name}"; shift 2 ;;
    --phases)  PHASES="${2:?--phases needs a value}"; shift 2 ;;
    --phases=*) PHASES="${1#--phases=}"; shift ;;
    --check-lib) CHECK_LIB=1; shift ;;
    --loop)    LOOP_FOREVER=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) sed -n '2,48p' "$0"; exit 0 ;;
    *)         echo "deploy_to_board: unknown argument '$1'" >&2; exit 2 ;;
  esac
done

# The app folder on the board defaults to the sketch name, so deploying
# phishtap lands in /home/arduino/ArduinoApps/phishtap/sketch.
[ -n "$APP" ] || APP="$SKETCH"
SKETCH_DIR="$REPO_ROOT/sketch/$SKETCH"
INO_NAME="$SKETCH.ino"

# --phases and --loop rewrite guards that only exist in hw_selftest. Refusing
# them outright is better than accepting them and silently pushing an unchanged
# sketch, which would look like the flags had worked.
if [ "$SKETCH" != "hw_selftest" ] && { [ -n "$PHASES" ] || [ "$LOOP_FOREVER" -eq 1 ]; }; then
  echo "deploy_to_board: --phases and --loop apply only to the hw_selftest sketch." >&2
  echo "  '$SKETCH' has no RUN_PHASE_* or PT_LOOP_FOREVER guards to rewrite." >&2
  exit 2
fi

# Allow an explicit adb path, since a SDK install is not always on PATH.
ADB="${ADB:-adb}"
if ! command -v "$ADB" >/dev/null 2>&1; then
  for candidate in \
      "$HOME/Android/Sdk/platform-tools/adb" \
      /usr/bin/adb ; do
    if [ -x "$candidate" ]; then ADB="$candidate"; break; fi
  done
fi
if ! command -v "$ADB" >/dev/null 2>&1 && [ ! -x "$ADB" ]; then
  echo "deploy_to_board: adb not found." >&2
  echo "  EndeavourOS: sudo pacman -S android-tools" >&2
  echo "  Or set ADB=/path/to/adb and re-run." >&2
  exit 2
fi

if [ ! -f "$SKETCH_DIR/$INO_NAME" ]; then
  echo "deploy_to_board: cannot find $SKETCH_DIR/$INO_NAME" >&2
  echo "  Available sketches:" >&2
  for d in "$REPO_ROOT"/sketch/*/; do
    [ -d "$d" ] && echo "    --sketch $(basename "$d")" >&2
  done
  exit 2
fi

REMOTE_DIR="/home/arduino/ArduinoApps/$APP/sketch"
APP_ROOT="/home/arduino/ArduinoApps/$APP"

# ── Optional phase override ────────────────────────────────────────────────
# hw_selftest.ino guards each RUN_PHASE_* with #ifndef, so passing -D on the
# compiler line would work, but App Lab does not expose extra build flags. The
# reliable route is a tiny generated header that defines them BEFORE the .ino's
# guards are reached, included from a staging copy. To keep this script honest
# and simple, we instead rewrite the guard defaults in a staging copy.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

cp "$SKETCH_DIR"/*.h "$STAGE"/ 2>/dev/null || true
cp "$SKETCH_DIR/$INO_NAME" "$STAGE/$INO_NAME"

if [ -n "$PHASES" ]; then
  for p in A L W B C1 C2 C3; do
    want=0
    if [ "$PHASES" = "all" ]; then
      want=1
    else
      IFS=',' read -ra _plist <<< "$PHASES"
      for item in "${_plist[@]}"; do
        norm="$(echo "$item" | tr '[:lower:]' '[:upper:]' | tr -d ' ')"
        [ "$norm" = "$p" ] && want=1
      done
    fi
    # Replace the default inside the #ifndef guard. Only the default line is
    # touched, so the guard itself still allows a -D override later.
    perl -0pi -e "s/(#ifndef RUN_PHASE_$p\n#define RUN_PHASE_$p )\\d+/\${1}$want/" \
      "$STAGE/$INO_NAME"
  done
  echo "deploy_to_board: phase selection set to '$PHASES'"
  grep -n '^#define RUN_PHASE' "$STAGE/$INO_NAME" | sed 's/^/    /'
fi

if [ "$LOOP_FOREVER" -eq 1 ]; then
  perl -0pi -e 's/(#ifndef PT_LOOP_FOREVER\n#define PT_LOOP_FOREVER )\d+/${1}1/' \
    "$STAGE/$INO_NAME"
  echo "deploy_to_board: PT_LOOP_FOREVER=1 (selftest will repeat indefinitely)"
  grep -n '^#define PT_LOOP_FOREVER' "$STAGE/$INO_NAME" | sed 's/^/    /'
fi

# ── Gate before pushing ────────────────────────────────────────────────────
# Never push something that will not compile. The syntax gate is cheap and it
# has already caught real defects twice.
echo "deploy_to_board: running the syntax gate first"
if ! "$REPO_ROOT/tools/syntax_check.sh" >/tmp/pt_syntax.$$.log 2>&1; then
  echo "deploy_to_board: REFUSING TO DEPLOY - syntax gate failed:" >&2
  cat /tmp/pt_syntax.$$.log >&2
  rm -f /tmp/pt_syntax.$$.log
  exit 1
fi
rm -f /tmp/pt_syntax.$$.log
echo "    syntax gate PASS"

if ! "$REPO_ROOT/tools/no_write_check.sh" >/tmp/pt_nw.$$.log 2>&1; then
  echo "deploy_to_board: REFUSING TO DEPLOY - no-write policy check failed:" >&2
  cat /tmp/pt_nw.$$.log >&2
  rm -f /tmp/pt_nw.$$.log
  exit 1
fi
rm -f /tmp/pt_nw.$$.log
echo "    no-write policy PASS"

# The host-side unit tests cover the Arduino-free modules (pt_json.h,
# pt_events.h) that the production firmware depends on. They only exist once
# sketch/phishtap is present, so the gate is conditional; when it does exist it
# is mandatory, because a ring buffer that loses an event is undiagnosable from
# the outside and must not reach the board untested.
if [ -x "$REPO_ROOT/tools/host_gate.sh" ] && [ -d "$REPO_ROOT/sketch/phishtap" ]; then
  echo "deploy_to_board: running the host-side unit tests"
  if ! "$REPO_ROOT/tools/host_gate.sh" >/tmp/pt_host.$$.log 2>&1; then
    echo "deploy_to_board: REFUSING TO DEPLOY - host tests failed:" >&2
    tail -40 /tmp/pt_host.$$.log >&2
    rm -f /tmp/pt_host.$$.log
    exit 1
  fi
  rm -f /tmp/pt_host.$$.log
  echo "    host unit tests PASS"
fi

# ── Optional library pre-flight ───────────────────────────────────────────
# arduino-cli compiles every .cpp in a library's src/, so one broken translation
# unit in a dependency fails the build even when the sketch never calls it. That
# is exactly what MFRC522 1.4.11 did on this core. Checking the pinned versions
# here costs a download but saves a confusing flash cycle.
if [ "$CHECK_LIB" -eq 1 ] && [ -f "$SKETCH_DIR/sketch.yaml" ]; then
  echo "deploy_to_board: pre-flighting libraries pinned in sketch.yaml"
  # Pull "Name (version)" pairs out of the libraries list.
  while IFS='|' read -r libname libver; do
    # The sed capture is greedy and leaves a trailing space on the name
    # ("MFRC522 |1.4.12"), which would not match anything in the library index.
    libname="$(echo "$libname" | tr -d '[:space:]')"
    libver="$(echo "$libver" | tr -d '[:space:]')"
    [ -n "$libname" ] || continue
    if ! "$REPO_ROOT/tools/check_lib.sh" "$libname" "$libver"; then
      echo "deploy_to_board: REFUSING TO DEPLOY - $libname $libver does not compile." >&2
      echo "  Pick a different version in sketch.yaml, or run tools/check_lib.sh" >&2
      echo "  with several versions to find one that builds." >&2
      exit 1
    fi
  done < <(sed -n 's/^[[:space:]]*-[[:space:]]*\(.*\)[[:space:]]*(\(.*\))[[:space:]]*$/\1|\2/p' \
             "$SKETCH_DIR/sketch.yaml")
fi

# ── Push ───────────────────────────────────────────────────────────────────
# The main sketch file must be named sketch.ino inside the app's sketch folder;
# that is the name App Lab and arduino-cli expect, and it is what the compiler
# error paths on the board refer to.
run() {
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "    DRY RUN: $*"
  else
    "$@"
  fi
}

echo "deploy_to_board: target $REMOTE_DIR"

if [ "$DRY_RUN" -eq 0 ]; then
  if ! "$ADB" get-state >/dev/null 2>&1; then
    echo "deploy_to_board: no device. Check the USB-C cable (data, not charge-only)" >&2
    echo "  and that udev rules are installed. Try: adb devices" >&2
    exit 1
  fi
  run "$ADB" shell "mkdir -p '$REMOTE_DIR'"
fi

# ── App Lab scaffold ───────────────────────────────────────────────────────
# App Lab discovers apps by the presence of app.yaml at the APP ROOT, not by the
# existence of a sketch folder. Pushing only sketch/ produces the worst of both
# worlds: the app does not appear in the Apps list, yet the directory is there,
# so "Create new app" rejects the name as already in use and there is no obvious
# way out from the UI.
#
# So create the four files App Lab expects. Each is written ONLY IF ABSENT, which
# matters for app.yaml above all: Stage 2 adds `bricks: [web_ui]` and
# `ports: [7000]` to it, and clobbering that on every deploy would silently
# remove the web server from the running app.
#
# Structure transcribed from a working app on this board:
#   ArduinoApps/<app>/app.yaml      name, description, icon, ports, bricks
#   ArduinoApps/<app>/.gitignore    ignores .cache/
#   ArduinoApps/<app>/README.md     generated by App Lab from app.yaml
#   ArduinoApps/<app>/python/main.py  the App.run entry point
#   ArduinoApps/<app>/sketch/       the .ino and headers
# .cache/ is generated by App Lab itself and must not be created here.

SCAFFOLD_APP_YAML="$STAGE/_app.yaml"
cat > "$SCAFFOLD_APP_YAML" <<YAML
# app.yaml: The main configuration file for your Arduino App.
# This file describes the application's metadata and properties.

# The user-visible name of the application.
name: $APP

# A brief description of what the application does.
description: "PhishTap Lite console firmware"

# The icon for the application, can be an emoji or a short string.
icon: 🎣

# A list of network ports that the application exposes.
# Example: [80, 443]
# Stage 2 will need [7000] here for the web_ui brick.
ports: []

# A list of bricks used by this application.
# Stage 2 will need [web_ui] here.
bricks: []
YAML

SCAFFOLD_README="$STAGE/_README.md"
cat > "$SCAFFOLD_README" <<MD
# 🎣 $APP

### Description

PhishTap Lite console firmware. See the repository's sketch/$SKETCH/README.md.
MD

SCAFFOLD_MAIN="$STAGE/_main.py"
cat > "$SCAFFOLD_MAIN" <<'PY'
"""PhishTap Lite - Python side entry point.

Stage 1b is firmware only, so this does nothing but keep the App framework alive.
That is deliberate and it has a visible consequence worth expecting: with no
Python polling the bridge, the MCU hears nothing for 15 seconds and enters its
link-lost state (panel blinking, RGB dark, error rhythm). That is the link-loss
safety working as designed, not a fault.

Stage 2 replaces this with the real backend: the bridge adapter that polls
pt.get_events, the game engine, SQLite and the web_ui brick.
"""

import time

from arduino.app_utils import App

print("PhishTap Lite: Stage 1b placeholder, firmware only")
print("  the console will report link-lost after 15s because nothing polls it")


def loop():
    """Called repeatedly by the App framework."""
    time.sleep(10)


# See: https://docs.arduino.cc/software/app-lab/tutorials/getting-started/#app-run
App.run(user_loop=loop)
PY

SCAFFOLD_GITIGNORE="$STAGE/_gitignore"
printf '# ignore app cache folder\n.cache/\n' > "$SCAFFOLD_GITIGNORE"

# Push each scaffold file only if the board does not already have one.
scaffold_push() {
  local local_file="$1" remote_path="$2" label="$3"
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "    DRY RUN: would create $remote_path if absent"
    return
  fi
  if "$ADB" shell "[ -e '$remote_path' ]" >/dev/null 2>&1; then
    echo "    scaffold: $label already exists, leaving it alone"
    return
  fi
  "$ADB" push "$local_file" "$remote_path" >/dev/null
  echo "    scaffold: created $label"
}

echo "deploy_to_board: ensuring the App Lab scaffold exists at $APP_ROOT"
if [ "$DRY_RUN" -eq 0 ]; then
  "$ADB" shell "mkdir -p '$APP_ROOT/python'"
fi

# app.yaml: the repository copy is AUTHORITATIVE and is always pushed.
#
# This differs from the other three scaffold files, and the difference is
# deliberate. app.yaml declares the bricks and published ports, so it is
# configuration that belongs in version control; letting a stale copy survive on
# the board would mean the running app does not match the repository, which is
# the kind of drift that produces "works on my board" and nothing else.
#
# The generated fallback below is only used when the repository has no app.yaml,
# which is the case for the hw_selftest diagnostic app.
if [ -f "$REPO_ROOT/app.yaml" ]; then
  echo "    scaffold: pushing the repository's app.yaml (authoritative)"
  run "$ADB" push "$REPO_ROOT/app.yaml" "$APP_ROOT/app.yaml"
else
  scaffold_push "$SCAFFOLD_APP_YAML" "$APP_ROOT/app.yaml" "app.yaml"
fi

scaffold_push "$SCAFFOLD_README"     "$APP_ROOT/README.md"       "README.md"
scaffold_push "$SCAFFOLD_GITIGNORE"  "$APP_ROOT/.gitignore"      ".gitignore"

# python/main.py: the repository copy wins, for the same reason as app.yaml.
#
# This matters more than it looks. The generated placeholder is create-if-absent,
# and an earlier deploy already put one on the board, so without this branch the
# real entry point would never be pushed and the app would keep running the
# do-nothing placeholder while looking like a successful deploy.
if [ -f "$REPO_ROOT/python/main.py" ]; then
  echo "    scaffold: pushing the repository's python/main.py (authoritative)"
  run "$ADB" push "$REPO_ROOT/python/main.py" "$APP_ROOT/python/main.py"
else
  scaffold_push "$SCAFFOLD_MAIN" "$APP_ROOT/python/main.py" "python/main.py"
fi

# The real Python package, when this repo has one. main.py is a thin entry point
# that imports the phishtap package, so pushing main.py alone would produce an
# ImportError at startup rather than a working app.
if [ -d "$REPO_ROOT/python/phishtap" ]; then
  echo "    scaffold: pushing the phishtap package and corpus"
  if [ "$DRY_RUN" -eq 0 ]; then
    # Clear the remote package first. adb push merges rather than replaces, so a
    # module deleted from the repository would otherwise survive on the board and
    # keep being importable, which is a confusing way to discover a stale file.
    "$ADB" shell "rm -rf '$APP_ROOT/python/phishtap'"
    "$ADB" shell "mkdir -p '$APP_ROOT/python/phishtap'"
    run "$ADB" push "$REPO_ROOT/python/phishtap/." "$APP_ROOT/python/phishtap/"
    # __pycache__ from the laptop would be for the wrong Python version, and a
    # mismatched .pyc is silently ignored at best and confusing at worst.
    run "$ADB" shell "find '$APP_ROOT/python' -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true"
  else
    echo "    DRY RUN: would push python/phishtap/"
  fi
fi
for extra in corpus.json requirements.txt; do
  if [ -f "$REPO_ROOT/python/$extra" ]; then
    run "$ADB" push "$REPO_ROOT/python/$extra" "$APP_ROOT/python/$extra"
  fi
done

run "$ADB" push "$STAGE/$INO_NAME" "$REMOTE_DIR/sketch.ino"

for h in "$STAGE"/*.h; do
  [ -e "$h" ] || continue
  run "$ADB" push "$h" "$REMOTE_DIR/$(basename "$h")"
done

# sketch.yaml carries the library list. App Lab manages it, but pushing ours
# keeps the MFRC522 dependency explicit and reproducible.
if [ -f "$SKETCH_DIR/sketch.yaml" ]; then
  run "$ADB" push "$SKETCH_DIR/sketch.yaml" "$REMOTE_DIR/sketch.yaml"
fi

# Files pushed over adb can land owned by root, which App Lab may then fail to
# read as the arduino user. Best-effort fix; ignore failure.
if [ "$DRY_RUN" -eq 0 ]; then
  "$ADB" shell "chown -R arduino:arduino '$APP_ROOT' 2>/dev/null || true" >/dev/null 2>&1 || true
fi

echo "deploy_to_board: done."
if [ "$DRY_RUN" -eq 0 ]; then
  # Confirm the manifest is actually there, because without it App Lab will not
  # list the app no matter how correct the sketch folder is.
  if "$ADB" shell "[ -f '$APP_ROOT/app.yaml' ]" >/dev/null 2>&1; then
    echo "  app.yaml present, so App Lab should list '$APP' in the Apps section."
    echo "  If it does not appear, reload the Apps list or refresh the browser tab."
  else
    echo "  WARNING: $APP_ROOT/app.yaml is missing. App Lab will NOT list this app." >&2
  fi
fi
echo "  Now press Run in App Lab and read the SERIAL MONITOR tab of the console."
echo "  (Not the Python or App Launch tab - those show the Linux side.)"
echo "  Open that tab BEFORE pressing Run, or use --loop so the run repeats."
exit 0
