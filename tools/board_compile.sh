#!/usr/bin/env bash
#
# PhishTap Lite — compile a sketch on the BOARD with the real Zephyr toolchain.
#
# WHY THIS EXISTS ALONGSIDE syntax_check.sh
# tools/syntax_check.sh compiles against hand-written stubs in tools/stubs/. That
# is fast and catches most mistakes, but a stub is only as good as whoever wrote
# it, and a stub that is more permissive than the real library will happily pass
# code the board then rejects.
#
# This script removes that gap. arduino-cli is installed on the board at
# /usr/bin/arduino-cli and the arduino:zephyr 1.1.0 core is present, so the sketch
# can be compiled by the ACTUAL toolchain against the ACTUAL libraries, with no
# stubs involved. It is the strongest verification available short of running the
# firmware, and it has already earned its keep: it confirmed that
# Arduino_LED_Matrix::begin() returns 1 unconditionally, which the stub also
# modelled but which is far more convincing read off the real build.
#
# WHAT IT DOES
#   1. runs deploy_to_board.sh, which itself runs the syntax gate, the no-write
#      policy gate and the host unit tests, then pushes the sketch over adb
#   2. runs `arduino-cli compile --fqbn arduino:zephyr:unoq` on the board
#   3. reports flash and RAM usage, and fails on any error
#
# It does NOT flash or run anything. Compiling writes only to a build directory,
# so this is safe to run as often as you like and cannot disturb a running app.
#
# USAGE
#   tools/board_compile.sh                     compile hw_selftest
#   tools/board_compile.sh --sketch phishtap   compile the production firmware
#   tools/board_compile.sh --skip-deploy       compile whatever is already pushed
#
# EXIT STATUS
#   0 compiled clean, 1 compile failed or a gate failed, 2 usage / no device

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKETCH="hw_selftest"
SKIP_DEPLOY=0
FQBN="arduino:zephyr:unoq"

while [ $# -gt 0 ]; do
  case "$1" in
    --sketch)      SKETCH="${2:?--sketch needs a name}"; shift 2 ;;
    --sketch=*)    SKETCH="${1#--sketch=}"; shift ;;
    --fqbn)        FQBN="${2:?--fqbn needs a value}"; shift 2 ;;
    --skip-deploy) SKIP_DEPLOY=1; shift ;;
    -h|--help)     sed -n '2,36p' "$0"; exit 0 ;;
    *)             echo "board_compile: unknown argument '$1'" >&2; exit 2 ;;
  esac
done

ADB="${ADB:-adb}"
if ! command -v "$ADB" >/dev/null 2>&1; then
  for candidate in "$HOME/Android/Sdk/platform-tools/adb" /usr/bin/adb; do
    if [ -x "$candidate" ]; then ADB="$candidate"; break; fi
  done
fi
if ! command -v "$ADB" >/dev/null 2>&1 && [ ! -x "$ADB" ]; then
  echo "board_compile: adb not found. Set ADB=/path/to/adb and re-run." >&2
  exit 2
fi

if ! "$ADB" get-state >/dev/null 2>&1; then
  echo "board_compile: no device attached." >&2
  echo "  Check the USB-C cable carries data, not just charge, then: adb devices" >&2
  exit 2
fi

REMOTE_DIR="/home/arduino/ArduinoApps/$SKETCH/sketch"

# ── Step 1: gates and push ────────────────────────────────────────────────
if [ "$SKIP_DEPLOY" -eq 0 ]; then
  echo "board_compile: running the host gates and pushing $SKETCH"
  if ! "$REPO_ROOT/tools/deploy_to_board.sh" --sketch "$SKETCH"; then
    echo "board_compile: REFUSING TO COMPILE - a host gate or the push failed." >&2
    exit 1
  fi
else
  echo "board_compile: --skip-deploy, compiling whatever is already at $REMOTE_DIR"
fi

# ── Step 2: compile with the real toolchain ───────────────────────────────
echo
echo "board_compile: compiling on the board with arduino-cli ($FQBN)"
echo "               this is the real Zephyr toolchain and the real libraries,"
echo "               not the host stubs. It can take a minute or two."

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

# stderr is merged in because arduino-cli reports some diagnostics there.
if "$ADB" shell "cd '$REMOTE_DIR' && arduino-cli compile --fqbn '$FQBN' . 2>&1" \
     > "$LOG" 2>&1; then
  RC=0
else
  RC=$?
fi

# The expected, documented, harmless warning. Filtered out of the error scan so
# it cannot mask a real failure, but still shown so nobody wonders where it went.
if grep -q 'claims to run on' "$LOG"; then
  echo "  note: the MFRC522 'incompatible architecture' warning appeared."
  echo "        That is expected and harmless; 1.4.12's library.properties omits"
  echo "        zephyr but the library is plain C++ over SPI and builds fine."
fi

if grep -qiE '^.*error:|Error during build|exit status [0-9]+' "$LOG"; then
  echo
  echo "board_compile: COMPILE FAILED"
  echo "────────────────────────────────────────────────────────────"
  grep -iE 'error|warning' "$LOG" | head -60
  echo "────────────────────────────────────────────────────────────"
  echo "Full log: $LOG"
  trap - EXIT
  exit 1
fi

if [ "$RC" -ne 0 ]; then
  echo "board_compile: arduino-cli exited $RC"
  tail -40 "$LOG"
  trap - EXIT
  exit 1
fi

echo
echo "──────── resource usage ────────"
grep -E 'program storage space|dynamic memory' "$LOG" | sed 's/^/  /'
echo "────────────────────────────────"

echo
echo "board_compile: PASS - $SKETCH compiles clean with the real toolchain."
echo
echo "What this PROVES:"
echo "  - the sketch builds against the actual arduino:zephyr 1.1.0 core and the"
echo "    actual installed libraries, with no stubs involved"
echo "  - every API it calls exists with the signature it assumes"
echo "  - it fits in flash and RAM, with the margins shown above"
echo
echo "What it does NOT prove:"
echo "  - that it behaves correctly. Compiling says nothing about whether the reader"
echo "    reads, the bridge binds, or the state machine advances."
echo "  - For that, press Run in App Lab and read the SERIAL MONITOR tab."
exit 0
