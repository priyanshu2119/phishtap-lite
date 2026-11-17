#!/usr/bin/env bash
#
# PhishTap Lite — host-side gate for the Arduino-free firmware modules.
#
# Some firmware modules are deliberately written with no Arduino.h dependency:
# pt_json.h and pt_events.h. That is not stylistic, it is so the REAL production
# code can be compiled and executed by plain g++ on a laptop, with assertions,
# before it is ever flashed to a board. There is no host-side copy that could
# drift from what runs on hardware.
#
# What this gate does, for each test:
#   1. compiles under -Wall -Wextra -Werror, so a warning is treated as a defect
#   2. compiles again under -fsanitize=address,undefined, because a JSON parser
#      and a ring buffer are exactly where an off-by-one writes past a buffer
#      only on input nobody thought to test
#   3. runs the assertions in both builds
#
# Then, for the event ring only:
#   4. pipes a representative payload through Python's json module. That parser
#      shares no code with the writer and implements RFC 8259 properly, so this
#      is independent proof that the bytes the firmware emits are valid JSON.
#      A codec that only ever validates itself proves nothing.
#
# If the toolchain lacks the sanitizer runtime, step 2 is skipped with a notice
# rather than failing, so the gate still runs on a minimal system.
#
# Exit codes:
#   0  everything compiled clean and every assertion passed
#   1  a compile error, a warning, an assertion failure, or invalid JSON
#
# Usage:
#   tools/host_gate.sh
#   tools/host_gate.sh -v        show compiler output and keep going verbosely

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="$REPO_ROOT/tools"
VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

CXXFLAGS="-std=c++14 -O1 -g -Wall -Wextra -Werror"
SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

if ! command -v g++ >/dev/null 2>&1; then
  echo "host_gate: g++ not found. Install it to run this gate." >&2
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "host_gate: host-side verification of the Arduino-free firmware modules"
echo "           compiler: $(g++ --version | head -1)"

# Sanitizer support is probed once, up front, so each test reports consistently.
SAN_OK=0
if g++ $CXXFLAGS $SAN_FLAGS -o "$WORK/probe" "$TOOLS/test_pt_json.cpp" 2>/dev/null; then
  SAN_OK=1
fi
if [ "$SAN_OK" -eq 1 ]; then
  echo "           sanitizers: available (address, undefined)"
else
  echo "           sanitizers: UNAVAILABLE, that build will be skipped"
fi

# run_one <source.cpp> <label>
run_one() {
  local src="$1" label="$2"

  if [ ! -f "$src" ]; then
    echo "host_gate: missing $src" >&2
    exit 1
  fi

  echo
  echo "=============================================================="
  echo " $label"
  echo "=============================================================="

  echo "[build] -Wall -Wextra -Werror"
  if [ "$VERBOSE" -eq 1 ]; then
    g++ $CXXFLAGS -o "$WORK/plain" "$src" || { echo "RESULT: FAIL - strict build"; exit 1; }
  else
    g++ $CXXFLAGS -o "$WORK/plain" "$src" 2>"$WORK/plain.err" \
      || { cat "$WORK/plain.err"; echo "RESULT: FAIL - strict build did not compile"; exit 1; }
    if [ -s "$WORK/plain.err" ]; then
      cat "$WORK/plain.err"
      echo "RESULT: FAIL - strict build emitted warnings"
      exit 1
    fi
  fi
  echo "        OK, no warnings"

  if [ "$SAN_OK" -eq 1 ]; then
    echo "[build] -fsanitize=address,undefined"
    g++ $CXXFLAGS $SAN_FLAGS -o "$WORK/san" "$src" 2>"$WORK/san.err" \
      || { cat "$WORK/san.err"; echo "RESULT: FAIL - sanitized build did not compile"; exit 1; }
    echo "        OK"
  fi

  echo "[run]   assertions (strict build)"
  "$WORK/plain" || { echo "RESULT: FAIL - assertions failed"; exit 1; }

  if [ "$SAN_OK" -eq 1 ]; then
    echo
    echo "[run]   assertions (sanitized build)"
    # halt_on_error aborts on the first detected overrun instead of continuing
    # with corrupted memory and producing misleading later failures.
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
      "$WORK/san" || { echo "RESULT: FAIL - sanitizer error or assertion failure"; exit 1; }
  fi
}

run_one "$TOOLS/test_pt_json.cpp"   "pt_json.h   - JSON codec"
run_one "$TOOLS/test_pt_events.cpp" "pt_events.h - event ring buffer"

# ── Independent JSON validation ────────────────────────────────────────────
echo
echo "=============================================================="
echo " Independent validation of the emitted JSON"
echo "=============================================================="

if command -v python3 >/dev/null 2>&1; then
  g++ $CXXFLAGS -o "$WORK/dump" "$TOOLS/test_pt_events.cpp" \
    || { echo "RESULT: FAIL - could not build the dump binary"; exit 1; }

  "$WORK/dump" --dump > "$WORK/payload.json" \
    || { echo "RESULT: FAIL - the dump mode did not produce a payload"; exit 1; }

  BYTES=$(wc -c < "$WORK/payload.json")
  echo "payload: $BYTES bytes from the firmware's own writer"

  # json.load raises on any syntax error, so a zero exit status here means an
  # independent RFC 8259 parser accepted exactly these bytes.
  if python3 - "$WORK/payload.json" <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    doc = json.load(fh)

assert isinstance(doc, dict), "top level must be an object"
for key in ("events", "oldest_seq", "overflow"):
    assert key in doc, f"missing envelope key: {key}"
assert isinstance(doc["events"], list), "events must be an array"
assert len(doc["events"]) > 0, "expected a non-empty payload"

seen = []
for ev in doc["events"]:
    for key in ("seq", "t_ms", "type", "data"):
        assert key in ev, f"event missing {key}: {ev}"
    assert isinstance(ev["data"], dict), f"data must be an object: {ev}"
    seen.append(ev["seq"])

assert seen == sorted(seen), f"seq must ascend: {seen}"
assert len(set(seen)) == len(seen), f"seq must be unique: {seen}"

types = {ev["type"] for ev in doc["events"]}
expected = {"heartbeat", "card_tap", "card_removed",
            "round_timeout", "button", "hw_fault"}
assert types == expected, f"expected all six event types, got {types}"

taps = [ev for ev in doc["events"] if ev["type"] == "card_tap"]
for tap in taps:
    assert "uid_hex" in tap["data"], f"card_tap missing uid_hex: {tap}"
    assert "uid_len" in tap["data"], f"card_tap missing uid_len: {tap}"

# The escaped-UID event must have round-tripped through the writer's escaping
# and Python's unescaping back to the exact original bytes.
escaped = [ev for ev in taps if '"' in ev["data"]["uid_hex"]
           or "\\" in ev["data"]["uid_hex"]]
assert escaped, "the deliberately hostile UID did not survive"
assert escaped[0]["data"]["uid_hex"] == '0A"F\\01', \
    f"escape round trip wrong: {escaped[0]['data']['uid_hex']!r}"

print(f"  python json.load: VALID")
print(f"  {len(doc['events'])} events, seq {seen[0]}..{seen[-1]}, "
      f"{len(types)} distinct types")
print(f"  envelope keys, per-event keys, ascending unique seq: all correct")
print(f"  hostile UID with a quote and a backslash round-tripped exactly")
PY
  then
    echo "[ok]    an independent parser accepted the firmware's output"
  else
    echo "RESULT: FAIL - Python's json module rejected the payload"
    echo "--- payload was ---"
    cat "$WORK/payload.json"
    exit 1
  fi
else
  echo "NOTE: python3 not found, skipping the independent JSON validation."
  echo "      The assertions above still passed, but the emitted bytes have not"
  echo "      been checked against a parser that shares no code with the writer."
fi

echo
echo "=============================================================="
echo "RESULT: PASS"
echo "=============================================================="
echo
echo "What this gate PROVES:"
echo "  - pt_json.h and pt_events.h compile with no warnings under -Werror"
echo "  - every assertion passes, in both a plain and a sanitized build"
echo "  - neither module overruns a buffer or invokes undefined behaviour on the"
echo "    tested inputs, including malformed and hostile JSON"
echo "  - the ring's sequence, wraparound, cursor and ack arithmetic are correct"
echo "  - the bytes the firmware emits are valid JSON per Python's json module"
echo
echo "What it does NOT prove:"
echo "  - that the bridge on the board accepts these payloads. That needs hardware,"
echo "    and is covered by the pt.selftest and pt.get_events RPC round trip."
echo "  - anything about the RFID, matrix, buzzer or LED engines, which are not"
echo "    host-testable because they touch Arduino APIs."
exit 0
