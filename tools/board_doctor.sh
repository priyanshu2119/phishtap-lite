#!/usr/bin/env bash
#
# PhishTap Lite — inspect the board over ADB
#
# WHY THIS EXISTS
#   "The sketch did nothing" has several completely different causes, and the
#   App Lab build log does not distinguish them:
#     - the phase was compiled out (RUN_PHASE_B=0), so no RFID code ran at all
#     - an older copy of the sketch is still on the board
#     - the library version pinned in sketch.yaml is not the one that got built
#     - the sketch ran fine but its Monitor output was read from the wrong
#       console tab
#   All four look identical from the laptop. This script prints the facts.
#
# USAGE
#   tools/board_doctor.sh                # app "hw_selftest"
#   tools/board_doctor.sh --app myapp
#
# EXIT STATUS
#   0 ran (findings are printed, not judged), 2 adb unavailable

set -uo pipefail

APP="hw_selftest"
while [ $# -gt 0 ]; do
  case "$1" in
    --app)     APP="${2:?--app needs a name}"; shift 2 ;;
    --app=*)   APP="${1#--app=}"; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *)         echo "board_doctor: unknown argument '$1'" >&2; exit 2 ;;
  esac
done

ADB="${ADB:-adb}"
if ! command -v "$ADB" >/dev/null 2>&1; then
  for candidate in "$HOME/Android/Sdk/platform-tools/adb" /usr/bin/adb; do
    if [ -x "$candidate" ]; then ADB="$candidate"; break; fi
  done
fi
if ! command -v "$ADB" >/dev/null 2>&1 && [ ! -x "$ADB" ]; then
  echo "board_doctor: adb not found. sudo pacman -S android-tools" >&2
  exit 2
fi

if ! "$ADB" get-state >/dev/null 2>&1; then
  echo "board_doctor: no device. Check the USB-C cable is a DATA cable" >&2
  echo "  (charge-only cables will not work) and that udev rules are installed." >&2
  echo "  Try: adb devices" >&2
  exit 2
fi

SKETCH_DIR="/home/arduino/ArduinoApps/$APP/sketch"

echo "════════ board_doctor: app '$APP' ════════"
echo ""

echo "── 1. Is the router service up? (Monitor output depends on it) ──"
"$ADB" shell 'systemctl is-active arduino-router 2>/dev/null || echo "NOT-ACTIVE"'
echo ""

echo "── 2. Files actually present in the sketch folder ──"
"$ADB" shell "ls -la '$SKETCH_DIR' 2>/dev/null || echo 'SKETCH DIR MISSING'"
echo ""

echo "── 3. Which phases are compiled in? ──"
echo "   (these are the values the build used; 0 means the phase does not exist"
echo "    in the binary at all, so no amount of wiring will make it run)"
"$ADB" shell "grep -n '^#define RUN_PHASE' '$SKETCH_DIR/sketch.ino' 2>/dev/null || echo 'no RUN_PHASE defines found'"
echo ""

echo "── 4. Which MFRC522 version is pinned? ──"
echo "   1.4.11 does NOT compile on this core; it must be 1.4.12."
"$ADB" shell "grep -n 'MFRC522' '$SKETCH_DIR/sketch.yaml' 2>/dev/null || echo 'no sketch.yaml or no MFRC522 entry'"
echo ""

echo "── 5. Which MFRC522 versions are actually unpacked on the board? ──"
"$ADB" shell 'ls -d /home/arduino/.arduino15/internal/MFRC522_* 2>/dev/null || echo "none unpacked"'
echo ""

echo "── 6. Is the deployed sketch.ino the current one? ──"
echo "   Checksums are compared with the RUN_PHASE_* values normalised, because"
echo "   deploy_to_board.sh --phases rewrites those defaults in the copy it pushes."
echo "   A difference here means genuinely stale code, not just a phase change."
# Strip the RUN_PHASE define lines before hashing so that selecting a different
# phase set does not look like a stale deployment.
norm() { grep -v '^#define RUN_PHASE_' | md5sum | cut -d' ' -f1; }
"$ADB" shell "cat '$SKETCH_DIR/sketch.ino' 2>/dev/null" | norm | sed 's/^/   board : /'
LOCAL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/sketch/$APP/hw_selftest.ino"
if [ -f "$LOCAL" ]; then
  norm < "$LOCAL" | sed 's/^/   local : /'
else
  echo "   local : (not found at $LOCAL)"
fi
echo "   Headers matter too - compare their timestamps in section 2 against"
echo "   your last deploy, since a stale header will not show up in this hash."
echo ""

echo "── 7. Core and bundled library versions ──"
"$ADB" shell 'ls /home/arduino/.arduino15/packages/arduino/hardware/zephyr/ 2>/dev/null | sed "s/^/   zephyr core: /"'
"$ADB" shell 'ls /home/arduino/.arduino15/packages/arduino/hardware/zephyr/*/libraries/ 2>/dev/null | sed "s/^/   bundled lib: /"'
echo ""

echo "── 8. Is ArduinoGraphics available? (would simplify matrix text) ──"
"$ADB" shell 'find /home/arduino/.arduino15/packages/arduino/hardware/zephyr -maxdepth 3 -iname "ArduinoGraphics*" 2>/dev/null | head -3 || true'
echo "   (no output above means it is NOT bundled)"
echo ""

echo "════════ how to read the sketch console ════════"
cat <<'EOF'
App Lab's console has three tabs, and the MCU output is in the third one:

    App Launch      -> container/provisioning messages
    Python          -> the Linux side (python/main.py): "Hello world!",
                       "App started". This is NOT the sketch.
    Serial monitor  -> THE MICROCONTROLLER. The selftest banner, the PHASE A/B
                       sections and the SUMMARY all appear here.

The tab shows a fixed 9600 baud and will not let you change it. That is
harmless: on this board Serial and Monitor are the same object
(variant.h defines ARDUINO_ROUTERBRIDGE_PROVIDES_SERIAL, and
Arduino_RouterBridge declares `Serial` as a GNU alias for `Monitor`), and
BridgeMonitor::begin() explicitly discards its baud argument. The channel is
virtual, carried over the router, so the baud rate is cosmetic.

If the Serial monitor tab is empty:
  1. Open the tab BEFORE pressing Run. The selftest prints most of its output
     from setup(), within the first seconds, and anything emitted before the tab
     is connected is lost. Or deploy with --loop so the run repeats forever.
  2. Confirm arduino-router is active (section 1 above).
  3. Restart the router and retry:
        adb shell 'sudo systemctl restart arduino-router'
EOF
