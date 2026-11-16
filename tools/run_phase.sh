#!/usr/bin/env bash
#
# PhishTap Lite — run one diagnostic phase
#
# A friendlier front end to deploy_to_board.sh for the bring-up sequence. It
# prints the wiring each phase needs and the decision table for interpreting the
# result BEFORE deploying, so the operator knows what they are looking at rather
# than reading a wall of console output cold.
#
# USAGE
#   tools/run_phase.sh L        SPI loopback. RC522 REMOVED, D11-D12 jumper.
#   tools/run_phase.sh W        MISO line state. RC522 wired.
#   tools/run_phase.sh B        Full RC522 test. RC522 wired.
#   tools/run_phase.sh A        Board only, no wiring.
#   tools/run_phase.sh C        Lives LEDs and buzzer (C1,C2,C3).
#   tools/run_phase.sh diag     The full RC522 investigation: L then W then B.
#   tools/run_phase.sh all      Every phase.
#
# OPTIONS
#   --loop        repeat forever, so the Serial monitor can be opened late
#   --no-deploy   print the guidance only, do not touch the board
#   --app NAME    target a different App Lab app (default hw_selftest)
#
# EXIT STATUS  0 ok, 2 bad usage, or whatever deploy_to_board.sh returned

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPLOY="$REPO_ROOT/tools/deploy_to_board.sh"

MODE=""
EXTRA_ARGS=()
APP="hw_selftest"

while [ $# -gt 0 ]; do
  case "$1" in
    --loop)      EXTRA_ARGS+=("--loop"); shift ;;
    --no-deploy) EXTRA_ARGS+=("__nodeploy__"); shift ;;
    --app)       APP="${2:?--app needs a name}"; shift 2 ;;
    --app=*)     APP="${1#--app=}"; shift ;;
    -h|--help)   sed -n '2,26p' "$0"; exit 0 ;;
    -*)          echo "run_phase: unknown option '$1'" >&2; exit 2 ;;
    *)           if [ -n "$MODE" ]; then echo "run_phase: only one mode allowed" >&2; exit 2; fi
                 MODE="$(echo "$1" | tr '[:lower:]' '[:upper:]')"; shift ;;
  esac
done

if [ -z "$MODE" ]; then
  echo "usage: tools/run_phase.sh {A|L|W|B|C|diag|all} [--loop] [--no-deploy]" >&2
  exit 2
fi

# Map the friendly mode names onto the phase list the sketch understands.
case "$MODE" in
  A)    PHASES="A" ;;
  L)    PHASES="L" ;;
  W)    PHASES="W" ;;
  B)    PHASES="B" ;;
  C)    PHASES="C1,C2,C3" ;;
  DIAG) PHASES="L,W,B" ;;
  ALL)  PHASES="all" ;;
  *)    echo "run_phase: unknown mode '$MODE'. Use A, L, W, B, C, diag or all." >&2
        exit 2 ;;
esac

rule() { printf '%s\n' "------------------------------------------------------------"; }

echo ""
rule
echo " PhishTap Lite - phase $MODE"
rule

case "$MODE" in
  A)
    cat <<'EOF'
WIRING: none. Board only.

WHAT IT TESTS
  The 8x13 LED matrix, the onboard RGB LEDs LED3 and LED4, and the Monitor
  logging path. These are all soldered to the board, so nothing here can be
  mis-wired. A failure means a faulty board or a faulty core, not a bad jumper.

DECISION TABLE
  matrix shows all 6 patterns      -> matrix OK
  a row or column stays dark       -> that scan line is dead, report it
  RGB shows red/green/blue/white   -> RGB OK and active-low confirmed
  a segment is inverted            -> the polarity assumption is wrong for this
                                      core; report it, it changes Stage 1b
EOF
    ;;

  L)
    cat <<'EOF'
WIRING: REMOVE ALL SEVEN RC522 WIRES. Then fit ONE jumper:

    D11 (MOSI)  ----->  D12 (MISO)

  Nothing else. The RC522 must be completely disconnected, because the point of
  this test is to prove the board's SPI peripheral works with no slave involved.

WHAT IT TESTS
  Whether SPI2 is really routed to D11/D12/D13 on this core. The board's
  variant.h says MOSI=D11 (PB15), MISO=D12 (PB14), SCK=D13 (PB13), SS=D10 (PB9),
  and the devicetree overlay enables SPI2 with zephyr,deferred-init and
  st,gpio-control. That is strong evidence, but it is evidence from a header file
  rather than from silicon, and this settles it.

  Patterns 0xA5, 0x5A, 0xFF, 0x00, 0x3C and a single-bit walk are echoed back at
  100 kHz, 1 MHz and 4 MHz, three times each. 0x00 and 0xFF are NOT counted as
  proof of anything, because a stuck line reproduces them by accident.

DECISION TABLE
  all patterns echo at all 3 clocks
      -> SPI2 and the pinmux are FINE. The fault is on the RC522 side. Go to
         the LED power test in the README, then Phase W.
  all 0x00 at every clock
      -> MISO stuck low with no slave attached, so the peripheral is not driving
         these pins. Check the jumper is really in D11 and D12, then investigate
         pinctrl, deferred-init, and whether the SPI object bound to SPI2.
  all 0xFF at every clock
      -> MISO stuck high. Same investigation, plus check for a pull-up winning
         because the pin is never driven.
  repeats disagree with each other
      -> intermittent contact. Re-seat the jumper and re-run.
  some clocks pass, others fail
      -> marginal contact rather than a configuration fault. Re-seat and re-run
         before concluding anything.
EOF
    ;;

  W)
    cat <<'EOF'
WIRING: RC522 connected, all seven wires. NO loopback jumper.

    RC522 VCC/3.3V -> 3V3      (NEVER the 5V pin)
    RC522 GND      -> GND
    RC522 RST      -> D5
    RC522 SDA/SS   -> D10
    RC522 SCK      -> D13
    RC522 MOSI     -> D11
    RC522 MISO     -> D12

  JANALOG HEADER WARNING: the pin order there is BOOT, IOREF, RESET, +3V3, +5V,
  GND, GND, VIN. The 3V3 pin sits BETWEEN RESET and +5V, so being one pin off
  either reboots the MCU, puts it into bootloader mode, or feeds 5V into a part
  rated for 2.5 to 3.3V. Read the silkscreen, do not count positions.

WHAT IT TESTS
  The electrical state of the MISO line with no clock running. With SS held HIGH
  the RC522 is deselected and must release MISO, so the reading is set by the
  STM32's own internal pull resistor. With SS held LOW the chip is selected and
  should drive the line. Comparing the two tells us whether the module is
  connected and powered at all.

  INPUT_PULLDOWN is used if the core provides it. Verified present on core 1.1.0
  (api/Common.h defines it as 0x3 and wiring_digital.cpp maps it to
  GPIO_PULL_DOWN), but the code guards it with #ifdef and reports which path it
  took, so a future core that drops it degrades instead of failing to build.

DECISION TABLE
  SS HIGH follows the pull, SS LOW stops following it (driven)
      -> BEST CASE. The module is powered, connected and responding to chip
         select. Wiring and power are fine, so the problem is at the protocol
         layer. Re-run Phase B and study B0c and B1b.
  reads 0 even with pull-up, in both SS states
      -> MISO is held low while the chip is deselected, which it must not do.
         Most likely the MODULE IS NOT POWERED: an unpowered RC522 clamps MISO
         low through its input ESD diode, which looks exactly like a short to
         ground from out here. Do the LED power test. Also check for a short to
         GND, or a module damaged by 5V on VCC.
  follows the pull in BOTH SS states (drive not confirmed)
      -> INCONCLUSIVE, and that is normal. The line is healthy (not stuck low,
         not stuck high), but this static probe cannot confirm the module drives
         MISO, because the MFRC522 only drives MISO after an address byte is
         clocked in. RUN PHASE B - it clocks a real transaction. Only if Phase B
         also reads 0x00 should you suspect unsoldered headers, swapped
         MOSI/MISO, or a missing SCK.
  reads 1 even with pull-down
      -> MISO held high. Look for a solder bridge or a jumper landing on IOREF,
         which is a 3V3 OUTPUT two pins away from 3V3 on JANALOG.
  mixed or drifting readings
      -> a floating line picking up noise. Bad contact somewhere on the MISO path.
EOF
    ;;

  B)
    cat <<'EOF'
WIRING: RC522 connected, all seven wires. Same table and the same JANALOG
warning as Phase W.

WHAT IT TESTS
  The full reader path, in an order designed so each step narrows the fault:
    B0a  ten registers read raw and compared against the reset values documented
         in the NXP MFRC522 data sheet Rev. 3.9. Read BEFORE PCD_Init, because
         PCD_Init overwrites ModeReg (0x3F to 0x3D) and RFCfgReg (0x48 to 0x70).
    B0b  VersionReg read 20 times, to separate a stuck line from a noisy one.
    B0c  the same read with manual chip select and with the library's, to test
         whether st,gpio-control behaves as the overlay declares.
    B0d  the clock sweep at 100 kHz, 1 MHz and 4 MHz.
    B1   VersionReg through the library.
    B1b  ModeReg and RFCfgReg read back after PCD_Init, to prove WRITES land and
         not just reads. A bus can read fine and still have a broken MOSI.
    B2   the chip's own self-test.
    B3   first UID read, timed.
    B4   five consecutive reads that must return an identical UID.
    B5   removal detection, which must use WUPA because a halted card ignores
         REQA.

DECISION TABLE
  B0a all ten match            -> chip alive and correctly wired
  B0a none match, all 0x00     -> bus or power dead. Run Phase W then Phase L.
  B0a some match               -> bus works but bits corrupt. If the differences
                                  are all in one bit position, suspect one line.
  B0b all 20 identical         -> stable, so a wrong value is stuck not noisy
  B0b values vary              -> contact or noise problem, re-seat jumpers
  B0c manual and library differ-> chip select handling is the fault
  B1b still at reset values    -> reads work but writes do not. Suspect MOSI.
  B2 fails, version 0x88       -> FM17522 clone. Expected, not a fault.
  B3 no card in 30s            -> the card may be 125 kHz, which the RC522 can
                                  never read. Test it with an Android phone's NFC.
  VersionReg 0x91 or 0x92      -> genuine MFRC522 v1.0 or v2.0
EOF
    ;;

  C)
    cat <<'EOF'
WIRING: lives LEDs on D7, D8, D9, then the buzzer on D6.

  Each LED:  pin -> 150R -> LED anode (long leg) -> cathode -> GND
  Blue or white LEDs need 68R instead of 150R or they are nearly invisible.

  Buzzer, TMB12A05 active, low-side NPN switch:
    D6 -> 1k -> transistor BASE
    transistor BASE -> 10k -> GND
    transistor EMITTER -> GND
    transistor COLLECTOR -> buzzer '-'
    buzzer '+' -> 5V pin
    1N4007 across the buzzer, CATHODE (silver stripe) to buzzer '+'

  TRANSISTOR PINOUT TRAP: with the flat face toward you, left to right, PN2222A is
  E-B-C but P2N2222A and BC547 are C-B-E. The base is the middle pin in all three
  and emitter/collector are swapped. Read the marking on the body, look up THAT
  marking, and confirm with a multimeter diode test before powering on. A hot
  transistor means it is backwards; power off immediately.

DECISION TABLE
  C1 LED blinks 5 times        -> D7 and the resistor are right
  C1 never lights              -> LED reversed (long leg is the anode), wrong
                                  resistor value, or blue/white LED needing 68R
  C2 chase then 3-2-1-0        -> all three lives LEDs work
  C2 one LED dark              -> that pin's wiring or polarity
  C2 two or more dark          -> the shared GND is not actually connected
  C3 all patterns audible      -> buzzer, transistor, diode and 5V rail all good
  C3 silent                    -> work the checklist the sketch prints: transistor
                                  orientation, buzzer polarity, diode orientation,
                                  5V rail present, and at least 4.0V across the
                                  buzzer while it should be sounding
EOF
    ;;

  DIAG)
    cat <<'EOF'
THE FULL RC522 INVESTIGATION, in the order that isolates the fault fastest.

  Phase L  RC522 REMOVED, one jumper D11 -> D12. Proves the board's SPI works.
  Phase W  RC522 wired. Proves the module is powered and connected.
  Phase B  RC522 wired. The full reader test.

You will be asked to change the wiring twice. Each change is announced on the
console AND scrolled on the LED matrix, then a countdown runs. The console cannot
take keystrokes, so the countdown is the only way to tell the board you are ready.

If Phase L passes and Phase W says the module is powered and driving MISO, but
Phase B still cannot read a register, the fault is at the protocol layer and the
B0c and B1b output is what to study.

Do the LED power test in the README FIRST if you have not already. It takes two
minutes and proves which physical header pins are really 3V3 and GND, which is the
thing most likely to be wrong given how crowded JANALOG is.
EOF
    ;;

  ALL)
    cat <<'EOF'
Every phase, in order: A, L, W, B, C1, C2, C3.

This needs the wiring changed several times and is mainly useful once the board is
fully assembled and you want a complete report. For bring-up, run the phases
individually or use 'diag' for the RC522 investigation.
EOF
    ;;
esac

rule
echo ""
echo "Console: read the SERIAL MONITOR tab in App Lab"
echo "         (called 'Sketch' in some App Lab versions)."
echo "         Not the Python tab, and not App Launch."
echo "         OPEN IT BEFORE PRESSING RUN, or use --loop."
echo ""

# Honour --no-deploy by filtering it out of the args we pass on.
DO_DEPLOY=1
PASS_ARGS=()
for a in "${EXTRA_ARGS[@]}"; do
  if [ "$a" = "__nodeploy__" ]; then DO_DEPLOY=0; else PASS_ARGS+=("$a"); fi
done

if [ "$DO_DEPLOY" -eq 0 ]; then
  echo "--no-deploy given, so nothing was pushed to the board."
  echo "To deploy: tools/deploy_to_board.sh --phases=$PHASES ${PASS_ARGS[*]:-}"
  exit 0
fi

echo "Deploying phases: $PHASES"
echo ""
exec "$DEPLOY" --phases="$PHASES" --app "$APP" ${PASS_ARGS[@]+"${PASS_ARGS[@]}"}
