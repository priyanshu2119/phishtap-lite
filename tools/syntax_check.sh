#!/usr/bin/env bash
#
# PhishTap Lite — host-side syntax gate
#
# WHAT IT DOES
#   Runs `g++ -fsyntax-only` over every sketch in sketch/ using the stub headers
#   in tools/stubs/, so real C++ errors are caught on the laptop instead of
#   costing a flash cycle on the board.
#
# WHY THIS EXISTS
#   Arduino's build only auto-generates function prototypes for the main .ino
#   file. Code in an included .h gets NO prototypes, so calling a function before
#   its definition is a hard compile error there even though the identical code
#   would compile if it lived in the .ino. That is a real class of bug in this
#   project, which is header-heavy, and it is invisible to reading the code.
#
# WHAT IT CATCHES
#   missing forward declarations, unknown identifiers, wrong argument counts and
#   types, calls to functions that do not exist in the stubbed API surface, and
#   collisions with the C library (the stub Arduino.h includes <math.h> for
#   exactly this reason).
#
# WHAT IT DOES *NOT* CATCH
#   Anything behavioural. The stubs have no implementation, so this proves the
#   code COMPILES, not that it WORKS. It cannot see a wrong pin number, an
#   inverted polarity, a bad resistor value or a logic error. Those are what the
#   on-board hw_selftest phases are for. It also cannot catch errors in APIs the
#   stubs model inaccurately — a stub is only as good as the source it was
#   transcribed from.
#
# THE .ino PURITY RULE (checked separately below, and learned the hard way)
#   arduino-cli runs ctags over the .ino and INSERTS A PROTOTYPE for every
#   function defined there, positioned before the rest of the file. If a helper's
#   signature uses a project-defined type (an enum, typedef or class) or a type
#   from a header included further down, the hoisted prototype cannot see it and
#   the board build dies with errors that make no sense against the source:
#       error: 'PtVerdict' was not declared in this scope
#       error: 'MFRC522' has not been declared
#       error: 'verdictName' redeclared as different kind of entity
#   g++ alone does NOT reproduce this, because g++ does not generate prototypes.
#   The only reliable fix is to keep function definitions out of the .ino: it may
#   define setup() and loop() and nothing else. All helpers live in headers,
#   which arduino-cli leaves untouched.
#
# USAGE
#   tools/syntax_check.sh              check every sketch under sketch/
#   tools/syntax_check.sh -v           also show the g++ command line
#
# EXIT STATUS
#   0  every sketch parsed cleanly
#   1  at least one sketch has errors, or violates the .ino purity rule
#   2  the harness itself could not run (no g++, no stubs, no sketches)

set -uo pipefail

VERBOSE=0
case "${1:-}" in
  "")       ;;
  -v)       VERBOSE=1 ;;
  -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
  *)        echo "syntax_check: unknown argument '$1'" >&2; exit 2 ;;
esac

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STUB_DIR="$REPO_ROOT/tools/stubs"
SKETCH_ROOT="$REPO_ROOT/sketch"

if ! command -v g++ >/dev/null 2>&1; then
  echo "syntax_check: g++ not found. On EndeavourOS: sudo pacman -S gcc" >&2
  exit 2
fi
if [ ! -d "$STUB_DIR" ]; then
  echo "syntax_check: stub directory missing at $STUB_DIR" >&2
  exit 2
fi
if [ ! -d "$SKETCH_ROOT" ]; then
  echo "syntax_check: no sketch/ directory at $SKETCH_ROOT" >&2
  exit 2
fi

# The Zephyr core builds sketches as C++17.
CXX_STD="gnu++17"

checked=0
failed=0
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

#
# Enforce the .ino purity rule described in the header comment.
#
# A file-scope function definition in C/C++ source formatted by this project
# starts at column 0 and ends that line with '{'. setup() and loop() are the only
# two permitted. Anything else means arduino-cli will hoist a prototype for it
# and the board build will fail in a way g++ cannot reproduce here.
#
check_ino_purity() {
  local file="$1"
  local bad
  bad="$(awk '
    /^[A-Za-z_].*\)[[:space:]]*\{[[:space:]]*$/ {
      line = $0
      if (line ~ /^[A-Za-z_].*[[:space:]]setup[[:space:]]*\(/) next
      if (line ~ /^[A-Za-z_].*[[:space:]]loop[[:space:]]*\(/)  next
      printf "    line %d: %s\n", NR, line
    }
  ' "$file")"

  if [ -n "$bad" ]; then
    echo "   FAIL (.ino purity)"
    echo "   This .ino defines functions other than setup()/loop()."
    echo "   arduino-cli hoists a prototype for each of them above the rest of"
    echo "   the file, so any that use a project-defined type or a type from a"
    echo "   later #include will fail to build ON THE BOARD with errors g++"
    echo "   cannot reproduce here. Move these into a header:"
    printf '%s\n' "$bad"
    return 1
  fi
  return 0
}

while IFS= read -r -d '' ino; do
  rel="${ino#"$REPO_ROOT"/}"
  sketch_dir="$(dirname "$ino")"
  base="$(basename "$ino" .ino)"
  cpp="$tmpdir/${base}_$$.cpp"

  # Arduino concatenates the .ino into a .cpp and prepends #include <Arduino.h>.
  # It also generates prototypes for functions defined IN THE .INO — but not for
  # anything in a header. Reproducing only the include, and deliberately not the
  # prototype generation, keeps this check at least as strict as the real build.
  {
    echo '#include <Arduino.h>'
    cat "$ino"
  } > "$cpp"

  checked=$((checked + 1))
  echo "── checking $rel"

  # Purity first: a violation is a guaranteed board-build failure even when the
  # g++ pass below is clean, so it must not be masked by a passing compile.
  if ! check_ino_purity "$ino"; then
    failed=$((failed + 1))
    continue
  fi

  cmd=(g++ -std="$CXX_STD" -fsyntax-only -Wall
       -I "$STUB_DIR" -I "$sketch_dir" "$cpp")
  [ "$VERBOSE" -eq 1 ] && printf '   %s\n' "${cmd[*]}"

  if output="$("${cmd[@]}" 2>&1)"; then
    echo "   OK"
  else
    failed=$((failed + 1))
    echo "   FAIL"
    # Strip the temp path so the reported locations point at real files, and
    # drop the stub include path noise. Line numbers in the concatenated file are
    # offset by one relative to the .ino because of the prepended include.
    printf '%s\n' "$output" \
      | sed -e "s|$cpp|$rel|g" \
            -e "s|$STUB_DIR/||g" \
      | sed -n '1,60p'
    echo "   (note: line numbers are +1 versus the .ino because the harness"
    echo "    prepends '#include <Arduino.h>', exactly as the Arduino build does)"
  fi
done < <(find "$SKETCH_ROOT" -type f -name '*.ino' -print0 | sort -z)

echo "───"
if [ "$checked" -eq 0 ]; then
  echo "syntax_check: no .ino files found under sketch/. Nothing was checked."
  exit 2
fi

echo "syntax_check: $checked sketch(es) checked, $failed failed."
if [ "$failed" -gt 0 ]; then
  echo "RESULT: FAIL"
  exit 1
fi
echo "RESULT: PASS (compiles; behaviour still unverified - run the on-board phases)"
exit 0