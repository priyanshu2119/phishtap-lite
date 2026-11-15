#!/usr/bin/env bash
#
# PhishTap Lite — NO-WRITE policy enforcer
#
# WHAT IT DOES
#   Scans first-party sources for any reference to an MFRC522 function or command
#   code that can modify a contactless card, and fails if one is found.
#
# WHY A GREP SCRIPT AND NOT A COMPILE-TIME GUARD
#   The MFRC522 library declares its card-mutating entry points as ordinary
#   public members. There is no portable way to make merely naming one a compile
#   error without forking the library, and a fork would have to be maintained
#   against upstream forever. A textual scan is the cheapest check that actually
#   catches a violation, it needs no toolchain, and it can run in CI on every
#   commit.
#
# WHY COMMENTS ARE STRIPPED FIRST
#   The NO-WRITE policy header in phase_b_rfid.h lists every forbidden symbol by
#   name, because a rule nobody can read is a rule nobody follows. Scanning raw
#   text would flag that documentation as a violation. Comments are removed
#   before matching so the policy can be stated without tripping the check.
#
# SCOPE
#   Only sketch/, python/, assets/ and tests/ are scanned. tools/ is excluded
#   because this script necessarily contains the patterns itself. The vendored
#   MFRC522 library legitimately declares these symbols and is never scanned.
#
# USAGE
#   tools/no_write_check.sh            # scan the repo
#   tools/no_write_check.sh --list     # also print every file scanned
#
# EXIT STATUS
#   0  no violations
#   1  at least one violation
#   2  the script could not run (missing directory, bad arguments)

set -uo pipefail

LIST_FILES=0
case "${1:-}" in
  "")            ;;
  --list)        LIST_FILES=1 ;;
  -h|--help)     sed -n '2,40p' "$0"; exit 0 ;;
  *)             echo "no_write_check: unknown argument '$1'" >&2; exit 2 ;;
esac

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ ! -d "$REPO_ROOT" ]; then
  echo "no_write_check: cannot resolve repo root" >&2
  exit 2
fi

# Every symbol that can alter card contents. Function names come from the public
# MFRC522 API; the PICC_CMD_* entries are the raw ISO14443/MIFARE command codes,
# which are included because a caller could bypass the helper functions and send
# one directly through PCD_TransceiveData.
FORBIDDEN=(
  MIFARE_Write
  MIFARE_Ultralight_Write
  MIFARE_SetUid
  MIFARE_OpenUidBackdoor
  MIFARE_UnbrickUidSector
  MIFARE_SetAccessBits
  MIFARE_Increment
  MIFARE_Decrement
  MIFARE_Restore
  MIFARE_Transfer
  MIFARE_SetValue
  PCD_NTAG216_AUTH
  PICC_CMD_MF_WRITE
  PICC_CMD_UL_WRITE
  PICC_CMD_MF_INCREMENT
  PICC_CMD_MF_DECREMENT
  PICC_CMD_MF_RESTORE
  PICC_CMD_MF_TRANSFER
)

SCAN_DIRS=(sketch python assets tests)

# Strip comments while preserving the line count, so grep line numbers still map
# onto the original file.
#
# mode=c     handles // and /* */ but KEEPS '#' lines, because in C a '#' starts
#            a preprocessor directive and "#define X MIFARE_Write" is a real
#            violation, not a comment.
# mode=hash  additionally treats '#' as a comment start, for Python and shell.
#
# Known limitation: a '//' inside a string literal is treated as a comment start.
# That can only truncate a line, and none of the forbidden symbols look like a
# URL, so it cannot hide a violation this script cares about.
strip_comments() {
  awk -v mode="$1" '
    BEGIN { inblock = 0 }
    {
      line = $0
      out  = ""
      n    = length(line)
      i    = 1
      while (i <= n) {
        c  = substr(line, i, 1)
        nx = (i < n) ? substr(line, i + 1, 1) : ""
        if (inblock) {
          if (c == "*" && nx == "/") { inblock = 0; i += 2 } else { i++ }
          continue
        }
        if (c == "/" && nx == "*") { inblock = 1; i += 2; continue }
        if (c == "/" && nx == "/") { break }
        if (mode == "hash" && c == "#") { break }
        out = out c
        i++
      }
      printf "%s\n", out
    }
  '
}

violations=0
scanned=0
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

for dir in "${SCAN_DIRS[@]}"; do
  [ -d "$REPO_ROOT/$dir" ] || continue

  while IFS= read -r -d '' file; do
    case "$file" in
      *.ino|*.h|*.hpp|*.c|*.cc|*.cpp) mode=c ;;
      *.py|*.sh)                      mode=hash ;;
      *.js|*.mjs|*.css)               mode=c ;;
      *)                              continue ;;
    esac

    scanned=$((scanned + 1))
    rel="${file#"$REPO_ROOT"/}"
    [ "$LIST_FILES" -eq 1 ] && echo "  scanning $rel"

    strip_comments "$mode" < "$file" > "$tmp"

    for pattern in "${FORBIDDEN[@]}"; do
      # -w so MIFARE_Write does not match inside a longer identifier, and so a
      # hit is a real reference rather than a substring of something else.
      while IFS= read -r hit; do
        [ -n "$hit" ] || continue
        lineno="${hit%%:*}"
        echo "VIOLATION: ${rel}:${lineno}: forbidden card-write symbol '${pattern}'"
        violations=$((violations + 1))
      done < <(grep -n -w -- "$pattern" "$tmp" 2>/dev/null)
    done
  done < <(find "$REPO_ROOT/$dir" -type f \
             \( -name '*.ino' -o -name '*.h' -o -name '*.hpp' \
                -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' \
                -o -name '*.py' -o -name '*.sh' \
                -o -name '*.js' -o -name '*.mjs' -o -name '*.css' \) \
             -print0 | sort -z)
done

echo "---"
if [ "$scanned" -eq 0 ]; then
  echo "no_write_check: WARNING - no source files found under ${SCAN_DIRS[*]}."
  echo "                Nothing was checked. This is not a pass."
  exit 2
fi

echo "no_write_check: scanned ${scanned} first-party file(s) across ${SCAN_DIRS[*]}."

if [ "$violations" -gt 0 ]; then
  echo "RESULT: FAIL - ${violations} violation(s)."
  echo "        This project is strictly read-only towards contactless cards."
  echo "        Remove every reference above. Reading a UID needs only"
  echo "        PICC_IsNewCardPresent, PICC_ReadCardSerial, PICC_HaltA and"
  echo "        PCD_StopCrypto1."
  exit 1
fi

echo "RESULT: PASS - no card-mutating API is referenced."
exit 0
