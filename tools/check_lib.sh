#!/usr/bin/env bash
#
# PhishTap Lite — third-party library pre-flight
#
# WHAT IT DOES
#   Downloads the exact .zip that App Lab / arduino-cli would install for a given
#   library name and version, then runs `g++ -fsyntax-only` over every .cpp in its
#   src/ using the stub headers in tools/stubs/.
#
# WHY
#   arduino-cli compiles EVERY .cpp in a library's src/, including files the
#   sketch never calls. A single broken translation unit therefore fails the whole
#   build, and the error points into library code the project does not own — which
#   makes it easy to misread as a wiring or configuration problem.
#
#   This happened for real: MFRC522 1.4.11 fails on the Zephyr core because
#   MFRC522Extended.cpp compares a `byte *` with `> 0`, an error in C++. The
#   project only uses the base MFRC522 class and never touches MFRC522Extended,
#   yet the build died. 1.4.12 dereferences the pointer and compiles clean.
#   Checking on the laptop costs seconds; discovering it on the board costs a
#   flash cycle and a confusing error log.
#
# USAGE
#   tools/check_lib.sh MFRC522 1.4.12
#   tools/check_lib.sh MFRC522 1.4.11 1.4.12      # compare versions
#   tools/check_lib.sh --owner miguelbalboa MFRC522 1.4.12
#
# EXIT STATUS
#   0 every requested version compiled cleanly
#   1 at least one version has errors
#   2 could not run (missing tool, bad arguments, download failed)

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STUB_DIR="$REPO_ROOT/tools/stubs"
OWNER=""
KEEP=0

while [ $# -gt 0 ]; do
  case "$1" in
    --owner)  OWNER="${2:?--owner needs a value}"; shift 2 ;;
    --owner=*) OWNER="${1#--owner=}"; shift ;;
    --keep)   KEEP=1; shift ;;
    -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
    -*)        echo "check_lib: unknown option '$1'" >&2; exit 2 ;;
    *)         break ;;
  esac
done

if [ $# -lt 2 ]; then
  echo "usage: tools/check_lib.sh [--owner <github-owner>] <LibraryName> <version> [version...]" >&2
  exit 2
fi

LIB="$1"; shift
VERSIONS=("$@")

for tool in curl unzip g++; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "check_lib: '$tool' not found. On EndeavourOS: sudo pacman -S curl unzip gcc" >&2
    exit 2
  fi
done

if [ ! -d "$STUB_DIR" ]; then
  echo "check_lib: stub directory missing at $STUB_DIR" >&2
  exit 2
fi

resolve_owner() {
  # If the caller did not name an owner, discover it from the library index so
  # the URL is not guessed. The index is large (~60 MB) but authoritative.
  if [ -n "$OWNER" ]; then
    printf '%s' "$OWNER"
    return 0
  fi
  local index="$REPO_ROOT/.cache/library_index.json"
  if [ ! -f "$index" ]; then
    mkdir -p "$(dirname "$index")"
    echo "check_lib: fetching the Arduino library index (one-off, ~60 MB)..." >&2
    if ! curl -sSL --max-time 300 \
        https://downloads.arduino.cc/libraries/library_index.json -o "$index"; then
      echo "check_lib: could not download the library index; pass --owner <github-owner>" >&2
      return 1
    fi
  fi
  python3 - "$index" "$LIB" <<'PY'
import json, sys
index_path, lib_name = sys.argv[1], sys.argv[2]
with open(index_path) as fh:
    data = json.load(fh)
# The index holds one entry per published version; take the newest match so the
# owner reflects the current maintainer rather than an early fork.
best = None
for entry in data.get("libraries", []):
    if entry.get("name") != lib_name:
        continue
    url = entry.get("url") or ""
    # URLs look like .../github.com/<owner>/<repo>-<version>.zip
    if "/github.com/" not in url:
        continue
    owner = url.split("/github.com/")[1].split("/")[0]
    version = entry.get("version") or ""
    key = tuple(int(p) for p in version.split(".") if p.isdigit())
    if best is None or key > best[0]:
        best = (key, owner)
if best is None:
    sys.exit(1)
print(best[1])
PY
}

OWNER_RESOLVED="$(resolve_owner)" || {
  echo "check_lib: could not resolve a GitHub owner for '$LIB'." >&2
  echo "           Re-run with --owner <github-owner>." >&2
  exit 2
}
echo "check_lib: library '$LIB', GitHub owner '$OWNER_RESOLVED'"

WORK="$(mktemp -d)"
cleanup() { [ "$KEEP" -eq 1 ] && echo "check_lib: kept work dir $WORK" || rm -rf "$WORK"; }
trap cleanup EXIT

overall_fail=0

for V in "${VERSIONS[@]}"; do
  echo ""
  echo "════════ $LIB $V ════════"
  zip="$WORK/$LIB-$V.zip"
  url="https://downloads.arduino.cc/libraries/github.com/$OWNER_RESOLVED/$LIB-$V.zip"
  echo "  GET $url"

  if ! curl -fsSL --max-time 120 "$url" -o "$zip"; then
    echo "  DOWNLOAD FAILED - is that version published in the Arduino index?"
    overall_fail=1
    continue
  fi

  if ! unzip -qo "$zip" -d "$WORK/$V"; then
    echo "  UNZIP FAILED"
    overall_fail=1
    continue
  fi

  # Report what the library claims to support, since a missing `zephyr` entry is
  # what produces the (harmless) incompatibility warning during the real build.
  props="$(find "$WORK/$V" -name library.properties -print -quit 2>/dev/null)"
  if [ -n "$props" ]; then
    echo "  library.properties:"
    grep -E '^(name|version|architectures)=' "$props" | sed 's/^/    /'
    if ! grep -qE '^architectures=.*(\*|zephyr)' "$props"; then
      echo "    NOTE: 'zephyr' is not listed, so App Lab will print an"
      echo "          incompatibility WARNING. That warning is not fatal."
    fi
  fi

  srcdir="$(find "$WORK/$V" -type d -name src -print -quit 2>/dev/null)"
  if [ -z "$srcdir" ]; then
    echo "  no src/ directory found - unexpected library layout"
    overall_fail=1
    continue
  fi

  version_fail=0
  while IFS= read -r -d '' cpp; do
    out="$(g++ -std=gnu++17 -fsyntax-only -I "$STUB_DIR" -I "$srcdir" "$cpp" 2>&1)"
    rel="${cpp#"$WORK/$V"/}"
    if [ -z "$out" ]; then
      echo "  CLEAN   $rel"
    else
      version_fail=1
      echo "  ERRORS  $rel"
      # Show only the error lines; the surrounding note/candidate spam from g++
      # is long and rarely changes the diagnosis.
      printf '%s\n' "$out" | grep -E "error:" | head -8 | sed 's/^/            /'
      nerr="$(printf '%s\n' "$out" | grep -cE 'error:')"
      [ "$nerr" -gt 8 ] && echo "            ... and $((nerr - 8)) more"
    fi
  done < <(find "$srcdir" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.c' \) -print0 | sort -z)

  if [ "$version_fail" -eq 0 ]; then
    echo "  RESULT: $LIB $V compiles cleanly against the stubs"
  else
    echo "  RESULT: $LIB $V FAILS - do not pin this version"
    overall_fail=1
  fi
done

echo ""
if [ "$overall_fail" -ne 0 ]; then
  echo "check_lib: one or more versions failed."
  echo "  Note: a failure here can also mean the STUBS are missing an API the library"
  echo "  uses, not that the library is broken. Compare against a version known to"
  echo "  build before concluding. 'ordered comparison of pointer with integer zero'"
  echo "  and similar plain-C++ diagnostics are always real."
  exit 1
fi
echo "check_lib: all requested versions compile cleanly."
exit 0
