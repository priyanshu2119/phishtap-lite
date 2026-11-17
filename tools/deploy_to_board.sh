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
  "$ADB" shell "chown -R arduino:arduino '$REMOTE_DIR' 2>/dev/null || true" >/dev/null 2>&1 || true
fi

echo "deploy_to_board: done."
echo "  Now press Run in App Lab and read the SERIAL MONITOR tab of the console."
echo "  (Not the Python or App Launch tab - those show the Linux side.)"
echo "  Open that tab BEFORE pressing Run, or use --loop so the run repeats."
exit 0
