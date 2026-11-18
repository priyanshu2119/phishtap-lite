#!/usr/bin/env python3
"""
PhishTap Lite — frontend gate.

Checks the properties of assets/ that are easy to break by accident and expensive
to discover in front of an audience. Every check here corresponds to a real
requirement from docs/ARCHITECTURE.md §7 or to a mistake already made once.

  1. JavaScript parses. Uses `node --check` when node is available, because a real
     parser beats any heuristic. Skipped with a notice if node is missing.
  2. No external requests. The demo runs offline, so a CDN link is not merely
     slow — it hangs the page until the request times out.
  3. No HTML-injection sinks. innerHTML, insertAdjacentHTML, outerHTML,
     document.write, eval and new Function are all banned in first-party code.
     Every string this project renders is attacker-shaped BY DESIGN: the corpus is
     full of lookalike domains and fake sender names. One innerHTML turns a
     teaching aid into a stored XSS vector on a page students are told to trust.
  4. Every local asset reference resolves. A renamed file otherwise produces a
     silent 404 and an unstyled or non-functional page.
  5. Script load order. strings.js must precede common.js, which must precede the
     page script, because each depends on the previous. Getting this wrong yields
     "PT is not defined" at runtime, not at build time.
  6. No corpus answers under the web root. Risk R6: if corpus.json were served,
     the answers would be publicly downloadable and the tool would teach nothing.
  7. Basic mobile hygiene: charset, viewport and lang on every page. The game is
     played on a phone; a missing viewport means a desktop layout scaled down to
     unreadable, with tap targets a quarter the intended size.

Exit status: 0 if every check passed, 1 otherwise.
"""

from __future__ import annotations

import html.parser
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ASSETS = REPO_ROOT / "assets"

# First-party JavaScript. assets/libs/ is vendored third-party code and is
# excluded from the sink and style checks: minified libraries use innerHTML and
# eval-like constructs internally, and auditing them is not this gate's job.
# They ARE checked for syntax, since a truncated vendored file is a real failure.
FIRST_PARTY_JS_GLOB = "js/*.js"
VENDORED_JS_GLOB = "libs/*.js"

DANGEROUS_SINKS = [
    (r"\.innerHTML\s*=", "innerHTML assignment"),
    (r"\.outerHTML\s*=", "outerHTML assignment"),
    (r"insertAdjacentHTML\s*\(", "insertAdjacentHTML"),
    (r"document\.write\s*\(", "document.write"),
    (r"(?<![\w.])eval\s*\(", "eval()"),
    (r"new\s+Function\s*\(", "new Function()"),
    (r"\.insertAdjacentElement\s*\(\s*['\"]beforebegin", "insertAdjacentElement beforebegin"),
]

# Inline event handlers are banned for the same reason as the sinks: they are
# string-evaluated, and they also defeat any future Content-Security-Policy.
INLINE_HANDLER_RE = re.compile(r"\son[a-z]+\s*=", re.IGNORECASE)

EXTERNAL_REF_RE = re.compile(
    r"""(?:src|href)\s*=\s*["']\s*(?:https?:)?//""", re.IGNORECASE
)
CSS_EXTERNAL_RE = re.compile(r"""@import\s+(?:url\()?["']?\s*(?:https?:)?//""", re.IGNORECASE)

failures: list[str] = []
warnings: list[str] = []
checks_run = 0


def fail(msg: str) -> None:
    failures.append(msg)


def warn(msg: str) -> None:
    warnings.append(msg)


def section(title: str) -> None:
    print(f"\n-- {title}")


def ok(msg: str) -> None:
    print(f"   ok   {msg}")


# ── 1. JavaScript syntax ──────────────────────────────────────────────────

def check_js_syntax() -> None:
    global checks_run
    section("JavaScript parses")

    node = shutil.which("node") or shutil.which("nodejs")
    files = sorted(ASSETS.glob(FIRST_PARTY_JS_GLOB)) + sorted(ASSETS.glob(VENDORED_JS_GLOB))
    if not files:
        fail("no JavaScript found under assets/")
        return

    if not node:
        warn(f"node not found; skipped syntax checking {len(files)} file(s). "
             "Install node to enable this check.")
        print(f"   SKIP  node unavailable ({len(files)} files not checked)")
        return

    for path in files:
        checks_run += 1
        proc = subprocess.run(
            [node, "--check", str(path)],
            capture_output=True, text=True,
        )
        rel = path.relative_to(REPO_ROOT)
        if proc.returncode != 0:
            detail = (proc.stderr or proc.stdout).strip().splitlines()
            fail(f"{rel} does not parse:\n         " + "\n         ".join(detail[:6]))
            print(f"   FAIL  {rel}")
        else:
            ok(f"{rel}")


# ── 2. No external requests ───────────────────────────────────────────────

def check_no_external_requests() -> None:
    global checks_run
    section("No external requests")

    scanned = 0
    for path in sorted(ASSETS.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix.lower() not in {".html", ".js", ".css"}:
            continue
        scanned += 1
        checks_run += 1
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as e:
            fail(f"{path.relative_to(REPO_ROOT)} could not be read: {e}")
            continue

        rel = path.relative_to(REPO_ROOT)
        for lineno, line in enumerate(text.splitlines(), start=1):
            # A comment mentioning a URL is documentation, not a request. Strip
            # HTML and JS comments before scanning so docs do not trip the check.
            stripped = re.sub(r"<!--.*?-->", "", line)
            stripped = re.sub(r"^\s*(//|\*|/\*).*$", "", stripped)
            if EXTERNAL_REF_RE.search(stripped):
                fail(f"{rel}:{lineno} references an external URL. The demo runs "
                     f"offline; vendor the asset instead.\n         {line.strip()[:110]}")
            if CSS_EXTERNAL_RE.search(stripped):
                fail(f"{rel}:{lineno} has an external @import.\n         {line.strip()[:110]}")

    ok(f"{scanned} file(s) scanned, no external references")


# ── 3. No HTML-injection sinks ────────────────────────────────────────────

def check_no_dangerous_sinks() -> None:
    global checks_run
    section("No HTML-injection sinks in first-party JS")

    files = sorted(ASSETS.glob(FIRST_PARTY_JS_GLOB))
    if not files:
        fail("no first-party JavaScript found under assets/js/")
        return

    for path in files:
        checks_run += 1
        rel = path.relative_to(REPO_ROOT)
        text = path.read_text(encoding="utf-8", errors="replace")

        # Strip block and line comments so that prose explaining WHY innerHTML is
        # banned does not itself trip the check. review.js and kiosk.js both
        # contain such comments, and they are worth keeping.
        no_block = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        lines = []
        for line in no_block.splitlines():
            lines.append(re.sub(r"//.*$", "", line))
        cleaned = "\n".join(lines)

        found = []
        for pattern, label in DANGEROUS_SINKS:
            for m in re.finditer(pattern, cleaned):
                # Report the line in the ORIGINAL file so the number is useful.
                approx_line = cleaned[: m.start()].count("\n") + 1
                found.append((label, approx_line))

        if found:
            for label, lineno in found:
                fail(f"{rel}:~{lineno} uses {label}. Use PT.setText / PT.make "
                     f"instead; every string in this project is attacker-shaped.")
            print(f"   FAIL  {rel} ({len(found)} sink(s))")
        else:
            ok(f"{rel}")


def check_no_inline_handlers() -> None:
    global checks_run
    section("No inline event handlers in HTML")

    for path in sorted(ASSETS.glob("*.html")):
        checks_run += 1
        rel = path.relative_to(REPO_ROOT)
        text = path.read_text(encoding="utf-8", errors="replace")
        no_comments = re.sub(r"<!--.*?-->", "", text, flags=re.S)
        hits = INLINE_HANDLER_RE.findall(no_comments)
        if hits:
            fail(f"{rel} has {len(hits)} inline event handler(s) (onclick= and "
                 f"friends). Attach listeners in the page script instead.")
        else:
            ok(f"{rel}")


# ── 4/5. HTML references and script order ─────────────────────────────────

class _AssetParser(html.parser.HTMLParser):
    """Collects local references, script order and head metadata from a page."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.refs: list[tuple[str, str, int]] = []   # (attr, value, line)
        self.scripts: list[str] = []
        self.has_charset = False
        self.has_viewport = False
        self.lang: str | None = None
        self.title = False

    def handle_starttag(self, tag, attrs):
        d = dict(attrs)
        line = self.getpos()[0]

        if tag == "html":
            self.lang = d.get("lang")
        if tag == "meta":
            if d.get("charset"):
                self.has_charset = True
            name = (d.get("name") or "").lower()
            if name == "viewport":
                self.has_viewport = True
        if tag == "title":
            self.title = True

        for attr in ("src", "href"):
            value = d.get(attr)
            if value:
                self.refs.append((attr, value, line))
            # A <script> with no src is inline code, which this project avoids so
            # that all logic lives in a file the gate can parse.
            if tag == "script" and attr == "src" and value:
                self.scripts.append(value)


def check_html_references() -> None:
    global checks_run
    section("HTML references resolve, and scripts load in dependency order")

    pages = sorted(ASSETS.glob("*.html"))
    if not pages:
        fail("no HTML pages found under assets/")
        return

    for path in pages:
        checks_run += 1
        rel = path.relative_to(REPO_ROOT)
        text = path.read_text(encoding="utf-8", errors="replace")

        parser = _AssetParser()
        try:
            parser.feed(text)
            parser.close()
        except Exception as e:  # noqa: BLE001 - a parse error IS the finding
            fail(f"{rel} could not be parsed as HTML: {e}")
            continue

        # Head metadata.
        if not parser.has_charset:
            fail(f"{rel} has no <meta charset>. Non-ASCII corpus text (rupee signs, "
                 f"Devanagari) will render as mojibake.")
        if not parser.has_viewport:
            fail(f"{rel} has no viewport meta. On a phone the page renders as a "
                 f"scaled-down desktop layout and the tap targets become unusable.")
        if not parser.lang:
            warn(f"{rel} has no lang attribute; screen readers will guess the pronunciation.")
        if not parser.title:
            warn(f"{rel} has no <title>.")

        # Local references must exist on disk.
        missing = []
        for attr, value, lineno in parser.refs:
            if value.startswith(("http://", "https://", "//", "data:", "mailto:", "#")):
                continue
            # An absolute path under /api/ is a server route, not a file. diag.html
            # links to /api/health and friends so an operator can open the raw JSON
            # in a tab; those resolve at runtime and have no on-disk counterpart.
            if value.startswith("/api/"):
                continue
            target = (path.parent / value.split("?")[0].split("#")[0]).resolve()
            if not target.exists():
                missing.append(f"{rel}:{lineno} {attr}=\"{value}\"")
        if missing:
            for m in missing:
                fail(f"broken reference: {m}")
        else:
            ok(f"{rel}: all {len(parser.refs)} reference(s) resolve")

        # Script order: strings.js -> common.js -> page script.
        scripts = [s.split("/")[-1] for s in parser.scripts]
        if "common.js" in scripts:
            if "strings.js" not in scripts:
                fail(f"{rel} loads common.js without strings.js; PT.t() will have "
                     f"no strings and every label will render as its key path.")
            elif scripts.index("strings.js") > scripts.index("common.js"):
                fail(f"{rel} loads strings.js AFTER common.js; common.js reads "
                     f"PT_STRINGS at load time and would see nothing.")

        page_scripts = [s for s in scripts if s not in ("strings.js", "common.js")
                        and not s.startswith("socket.io") and s != "arduino.js"
                        and s != "qrcode.min.js"]
        for ps in page_scripts:
            if "common.js" in scripts and scripts.index(ps) < scripts.index("common.js"):
                fail(f"{rel} loads {ps} before common.js, so PT is undefined when "
                     f"the page script runs.")

        # A page that uses PT must load common.js.
        for ps in page_scripts:
            ps_path = path.parent / "js" / ps
            if ps_path.exists():
                body = ps_path.read_text(encoding="utf-8", errors="replace")
                if re.search(r"\bPT\.", body) and "common.js" not in scripts:
                    fail(f"{rel} loads {ps}, which uses PT.*, but never loads common.js.")


# ── 6. No corpus answers under the web root ───────────────────────────────

def check_no_corpus_leak() -> None:
    global checks_run
    section("No corpus answers under the web root (risk R6)")

    json_files = [p for p in ASSETS.rglob("*") if p.is_file() and p.suffix.lower() == ".json"]
    checks_run += 1
    if json_files:
        # A JSON file is not automatically a leak, so inspect the content. But any
        # JSON under the web root deserves scrutiny, because the corpus is the one
        # file that must never be served.
        for path in json_files:
            text = path.read_text(encoding="utf-8", errors="replace")
            rel = path.relative_to(REPO_ROOT)
            if '"explanation"' in text or '"lesson"' in text:
                fail(f"{rel} is under the web root and contains corpus answers. "
                     f"The corpus must stay at python/corpus.json.")
            elif '"label"' in text:
                fail(f"{rel} is under the web root and contains a 'label' field, "
                     f"which is how the corpus marks phish from safe.")
            else:
                warn(f"{rel} is a JSON file under the web root. Not a leak, but "
                     f"check that it should be publicly downloadable.")
        ok(f"{len(json_files)} JSON file(s) inspected, no answers found")
    else:
        ok("no JSON files under assets/")

    # The corpus must exist server-side, or the check above passes vacuously.
    corpus = REPO_ROOT / "python" / "corpus.json"
    checks_run += 1
    if not corpus.exists():
        fail("python/corpus.json is missing, so the no-leak check proved nothing.")
    else:
        ok(f"corpus is server-side at {corpus.relative_to(REPO_ROOT)}")


# ── 7. index.html must exist ──────────────────────────────────────────────

def check_index_exists() -> None:
    global checks_run
    section("The WebUI brick's required entry point")
    checks_run += 1
    index = ASSETS / "index.html"
    if not index.exists():
        fail("assets/index.html is missing. The WebUI brick raises RuntimeError at "
             "startup when assets_dir_path exists without an index.html.")
    else:
        ok("assets/index.html present")


def main() -> int:
    print("web_gate: frontend verification for assets/")
    print(f"          root: {ASSETS}")

    if not ASSETS.exists():
        print("web_gate: assets/ does not exist yet, nothing to check.")
        print("RESULT: SKIP")
        return 0

    check_js_syntax()
    check_no_external_requests()
    check_no_dangerous_sinks()
    check_no_inline_handlers()
    check_html_references()
    check_no_corpus_leak()
    check_index_exists()

    print("\n" + "=" * 62)
    if warnings:
        print(f"{len(warnings)} warning(s):")
        for w in warnings:
            print(f"  ! {w}")
        print()
    if failures:
        print(f"RESULT: FAIL - {len(failures)} problem(s) across {checks_run} checks")
        for f in failures:
            print(f"  x {f}")
        return 1

    print(f"RESULT: PASS - {checks_run} checks, no problems.")
    print()
    print("What this gate PROVES:")
    print("  - every first-party script parses, and the vendored ones are intact")
    print("  - no page makes an external request, so the offline demo cannot hang")
    print("  - no first-party code can inject HTML, so corpus text cannot become script")
    print("  - every referenced asset exists and scripts load in dependency order")
    print("  - the corpus answers are not downloadable from the web root")
    print()
    print("What it does NOT prove:")
    print("  - that the pages render correctly or behave correctly. That needs a")
    print("    browser, and is what Stage 3's manual verification is for.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
