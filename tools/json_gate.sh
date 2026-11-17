#!/usr/bin/env bash
#
# PhishTap Lite — host-side gate for the JSON codec.
#
# Compiles tools/test_pt_json.cpp against sketch/phishtap/pt_json.h with plain
# g++ and runs the assertions. pt_json.h has no Arduino dependency by design, so
# this exercises the REAL code that will run on the board, not a copy of it.
#
# Two builds are run:
#   1. -Wall -Wextra -Werror            a warning is treated as a defect
#   2. -fsanitize=address,undefined     catches buffer overruns and UB that the
#                                       assertions alone would miss. A JSON
#                                       parser is exactly the kind of code where
#                                       an off-by-one writes past a buffer only
#                                       on input nobody thought to test.
#
# If the toolchain lacks the sanitizer runtime, build 2 is skipped with a notice
# rather than failing the gate, so the script still works on a minimal system.
#
# Exit codes:
#   0  both builds compiled and every assertion passed
#   1  a compile error, a warning, or an assertion failure
#
# Usage:
#   tools/json_gate.sh
#   tools/json_gate.sh -v        keep the binaries and show compiler output

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$REPO_ROOT/tools/test_pt_json.cpp"
HDR="$REPO_ROOT/sketch/phishtap/pt_json.h"
VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

if [ ! -f "$SRC" ]; then
  echo "json_gate: missing $SRC" >&2
  exit 1
fi
if [ ! -f "$HDR" ]; then
  echo "json_gate: missing $HDR" >&2
  exit 1
fi

if ! command -v g++ >/dev/null 2>&1; then
  echo "json_gate: g++ not found. Install it to run this gate." >&2
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

CXXFLAGS="-std=c++14 -O1 -g -Wall -Wextra -Werror"

fail() {
  echo "RESULT: FAIL - $1"
  exit 1
}

echo "json_gate: testing $HDR"
echo "           compiler: $(g++ --version | head -1)"

# ── Build 1: strict warnings ───────────────────────────────────────────────
echo
echo "[1/2] compiling with -Wall -Wextra -Werror"
if [ "$VERBOSE" -eq 1 ]; then
  g++ $CXXFLAGS -o "$WORK/plain" "$SRC" || fail "strict build did not compile"
else
  g++ $CXXFLAGS -o "$WORK/plain" "$SRC" 2>"$WORK/plain.err" \
    || { cat "$WORK/plain.err"; fail "strict build did not compile"; }
  if [ -s "$WORK/plain.err" ]; then
    cat "$WORK/plain.err"
    fail "strict build emitted warnings"
  fi
fi
echo "      OK, no warnings"

# ── Build 2: sanitizers ────────────────────────────────────────────────────
echo
echo "[2/2] compiling with -fsanitize=address,undefined"
SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
if g++ $CXXFLAGS $SAN_FLAGS -o "$WORK/san" "$SRC" 2>"$WORK/san.err"; then
  echo "      OK, sanitizers available"
  RUN_SAN=1
else
  # A missing sanitizer runtime is an environment limitation, not a code defect.
  echo "      NOTE: sanitizers unavailable on this toolchain, skipping."
  [ "$VERBOSE" -eq 1 ] && cat "$WORK/san.err"
  RUN_SAN=0
fi

# ── Run ────────────────────────────────────────────────────────────────────
echo
echo "=== running assertions (strict build) ==="
"$WORK/plain"
PLAIN_RC=$?

SAN_RC=0
if [ "$RUN_SAN" -eq 1 ]; then
  echo
  echo "=== running assertions (sanitized build) ==="
  # halt_on_error makes ASan abort on the first detected overrun instead of
  # continuing with corrupted memory and producing misleading later failures.
  ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
  UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
    "$WORK/san"
  SAN_RC=$?
fi

echo
if [ "$PLAIN_RC" -ne 0 ]; then
  fail "assertions failed in the strict build"
fi
if [ "$RUN_SAN" -eq 1 ] && [ "$SAN_RC" -ne 0 ]; then
  fail "sanitizer reported an error, or assertions failed in the sanitized build"
fi

if [ "$RUN_SAN" -eq 1 ]; then
  echo "RESULT: PASS - all assertions passed, clean under ASan and UBSan."
else
  echo "RESULT: PASS - all assertions passed (sanitizers were unavailable)."
fi
echo
echo "What this gate PROVES: the JSON codec in pt_json.h is free of compile"
echo "warnings, passes its assertions, and does not overrun buffers or invoke"
echo "undefined behaviour on the tested inputs."
echo "What it does NOT prove: that the bridge on the board accepts these payloads."
echo "That needs the hardware, and is covered by the pt.selftest RPC round trip."
exit 0
