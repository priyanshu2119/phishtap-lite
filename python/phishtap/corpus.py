"""
PhishTap Lite — corpus loading, validation and defanging.

The corpus is the 40 phishing/safe messages the game shows. It lives
SERVER-SIDE at ``python/corpus.json`` and is never placed under ``assets/``,
because anything under assets is web-served and the answers would be publicly
downloadable. ``tests/test_corpus.py`` asserts that.

Two jobs here beyond loading:

VALIDATION. A malformed corpus item produces a confusing failure much later,
when a student is mid-round and the engine cannot find a field. Validating at
load time turns that into a startup error with the item's id in it.

DEFANGING. Every URL in a message is neutralised before it leaves the server:
``https://`` becomes ``hxxps://`` and the dots in a hostname become ``[.]``.
The frontend then renders message bodies as plain text, never as anchors. This
is defence in depth — the content is authored to be harmless, using only
reserved TLDs like ``.example`` and ``.invalid``, but a training tool about
phishing must not itself become a way to click through to a live malicious URL
if someone later edits the corpus carelessly.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence

from .config import CORPUS_PATH

# ── Vocabulary ────────────────────────────────────────────────────────────

CHANNELS: frozenset[str] = frozenset({"email", "sms", "whatsapp", "poster"})
DIFFICULTIES: frozenset[str] = frozenset({"easy", "hard"})

# label: 1 = phish, 0 = safe. Stored as an int in the DB so the answers table
# can compare with a single integer equality rather than a string match.
LABEL_PHISH = 1
LABEL_SAFE = 0

REQUIRED_FIELDS: tuple[str, ...] = (
    "id", "channel", "label", "difficulty",
    "sender", "subject", "body", "cues",
    "explanation", "lesson",
)


@dataclass(frozen=True)
class CorpusItem:
    """One message the game can show.

    Frozen and hashable by id so it can be put in sets and used as a dict key
    without accidental mutation. ``cues`` is a tuple rather than a list for the
    same reason.
    """

    id: str
    channel: str
    label: int                 # LABEL_PHISH or LABEL_SAFE
    difficulty: str            # "easy" or "hard"
    sender: str
    subject: str
    body: str
    cues: tuple[str, ...]
    explanation: str
    lesson: str

    @property
    def is_phish(self) -> bool:
        return self.label == LABEL_PHISH


# ── Defanging ─────────────────────────────────────────────────────────────

# A URL scheme. Case-insensitive, and matched with a word boundary so "myhttps"
# is not rewritten.
_SCHEME_RE = re.compile(r"\b(https?|ftp)://", re.IGNORECASE)

# A hostname. Requires the final label to be alphabetic and at least two
# characters, which is what keeps this from mangling ordinary prose:
#   "e.g."   -> final label "g" is one char, not matched
#   "1.5"    -> final label "5" is not alphabetic, not matched
#   "bank.example" -> matched, becomes "bank[.]example"
# A false positive here is harmless (a slightly odd-looking word); a false
# negative would leave a clickable-looking URL in a phishing trainer.
_HOST_RE = re.compile(
    r"\b([a-z0-9](?:[a-z0-9-]*[a-z0-9])?"   # first label
    r"(?:\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)*"  # middle labels
    r"\.[a-z]{2,24})\b",                     # final label: alphabetic, 2-24
    re.IGNORECASE,
)


def defang(text: str) -> str:
    """Neutralise every URL in ``text``.

    ``https://`` -> ``hxxps://`` and every dot inside a hostname -> ``[.]``,
    exactly as docs/ARCHITECTURE.md §7 specifies. Applied to sender, subject,
    body and explanation, because a URL can appear in any of them.

    The scheme is rewritten by an explicit mapping rather than by blanket
    character substitution. Substituting every 'p' with 'x' turns "https" into
    "httxs", which is defanged but does not match the documented form and reads
    like a typo. The mapping keeps the recognisable shape so a student can still
    see what the original scheme was, which is the point of showing them a
    defanged URL in a training tool.

    Idempotent: running it twice does not double-defang, because ``hxxps://`` no
    longer matches the scheme pattern and ``[.]`` no longer matches the hostname
    pattern.
    """
    if not text:
        return text

    # Lowercased scheme -> defanged form. Case is restored from the original
    # match so "HTTPS://" stays visually uppercase.
    _SCHEME_MAP = {"http": "hxxp", "https": "hxxps", "ftp": "fxp"}

    def _scheme_sub(m: re.Match[str]) -> str:
        original = m.group(1)
        out = _SCHEME_MAP.get(original.lower(), original)
        if original.isupper():
            out = out.upper()
        return out + "://"

    def _host_sub(m: re.Match[str]) -> str:
        return m.group(1).replace(".", "[.]")

    # Defang hosts first, then schemes. The two passes are independent: the host
    # regex requires an alphabetic final label so it never matches the "https"
    # part of a scheme, and the scheme regex stops at "://" so it never consumes
    # the hostname.
    text = _HOST_RE.sub(_host_sub, text)
    text = _SCHEME_RE.sub(_scheme_sub, text)
    return text


# ── Loading and validation ────────────────────────────────────────────────

class CorpusError(ValueError):
    """Raised when the corpus is missing, unreadable or invalid."""


def _coerce_item(raw: object, index: int) -> CorpusItem:
    """Validate and convert one raw JSON object into a CorpusItem."""
    where = f"item #{index}"
    if not isinstance(raw, dict):
        raise CorpusError(f"{where}: expected an object, got {type(raw).__name__}")

    item_id = raw.get("id")
    if isinstance(item_id, str) and item_id:
        where = f"item '{item_id}'"

    missing = [f for f in REQUIRED_FIELDS if f not in raw]
    if missing:
        raise CorpusError(f"{where}: missing field(s): {', '.join(missing)}")

    channel = raw["channel"]
    if channel not in CHANNELS:
        raise CorpusError(
            f"{where}: channel '{channel}' is not one of {sorted(CHANNELS)}"
        )

    label = raw["label"]
    if isinstance(label, bool):
        # bool is a subclass of int in Python, so True would silently pass an
        # isinstance(label, int) check and become 1 (phish). Reject explicitly.
        raise CorpusError(f"{where}: label must be 0 or 1, not a boolean")
    if not isinstance(label, int) or label not in (LABEL_SAFE, LABEL_PHISH):
        raise CorpusError(f"{where}: label must be 0 (safe) or 1 (phish), got {label!r}")

    difficulty = raw["difficulty"]
    if difficulty not in DIFFICULTIES:
        raise CorpusError(
            f"{where}: difficulty '{difficulty}' is not one of {sorted(DIFFICULTIES)}"
        )

    cues = raw["cues"]
    if not isinstance(cues, list) or not all(isinstance(c, str) for c in cues):
        raise CorpusError(f"{where}: cues must be a list of strings")

    for fname in ("sender", "subject", "body", "explanation", "lesson"):
        if not isinstance(raw[fname], str):
            raise CorpusError(f"{where}: {fname} must be a string")

    return CorpusItem(
        id=str(item_id),
        channel=channel,
        label=label,
        difficulty=difficulty,
        sender=raw["sender"],
        subject=raw["subject"],
        body=raw["body"],
        cues=tuple(cues),
        explanation=raw["explanation"],
        lesson=raw["lesson"],
    )


def load_corpus(path: Path | str = CORPUS_PATH) -> list[CorpusItem]:
    """Load and validate the corpus. Raises CorpusError on any problem.

    Defanging is applied at load time, once, rather than on every transmission.
    That is both faster and safer: there is no code path that can accidentally
    serve an undefanged body, because the in-memory items never held one.
    """
    p = Path(path)
    if not p.exists():
        raise CorpusError(f"corpus not found at {p}")

    try:
        raw = json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise CorpusError(f"corpus at {p} is not valid JSON: {e}") from e

    if not isinstance(raw, list):
        raise CorpusError(f"corpus at {p} must be a JSON array of items")

    items = [_coerce_item(r, i) for i, r in enumerate(raw)]

    ids = [it.id for it in items]
    dupes = {i for i in ids if ids.count(i) > 1}
    if dupes:
        raise CorpusError(f"corpus has duplicate id(s): {', '.join(sorted(dupes))}")

    # Defang every text field once, at load time.
    defanged: list[CorpusItem] = []
    for it in items:
        defanged.append(
            CorpusItem(
                id=it.id,
                channel=it.channel,
                label=it.label,
                difficulty=it.difficulty,
                sender=defang(it.sender),
                subject=defang(it.subject),
                body=defang(it.body),
                cues=tuple(defang(c) for c in it.cues),
                explanation=defang(it.explanation),
                lesson=defang(it.lesson),
            )
        )

    return defanged


def corpus_summary(items: Sequence[CorpusItem]) -> dict[str, object]:
    """Counts used by the validator and by the admin panel.

    Reported rather than enforced as hard errors, because a corpus that is
    slightly unbalanced is still playable, while a corpus that fails to load is
    not. The test suite asserts the intended 40/50-50/20-20 shape so a
    regression is caught, but the runtime only warns.
    """
    total = len(items)
    phish = sum(1 for i in items if i.is_phish)
    easy = sum(1 for i in items if i.difficulty == "easy")
    by_channel: dict[str, int] = {}
    for i in items:
        by_channel[i.channel] = by_channel.get(i.channel, 0) + 1
    return {
        "total": total,
        "phish": phish,
        "safe": total - phish,
        "easy": easy,
        "hard": total - easy,
        "by_channel": by_channel,
    }


# ── Item selection ────────────────────────────────────────────────────────

def build_session_pool(
    items: Sequence[CorpusItem],
    rng,
    total_rounds: int | None = None,
) -> list[CorpusItem]:
    """Build the ordered list of items for one session.

    Per docs/ARCHITECTURE.md §6: early rounds draw from ``easy``, later rounds
    from ``hard``, sampled without replacement, and balanced across channels so
    the same format does not repeat consecutively when the pool allows.

    ``rng`` is a ``random.Random`` instance supplied by the caller, so a session
    can be made reproducible in tests by passing a seeded generator. Taking it
    as a parameter rather than using the module-level ``random`` is what makes
    the selection testable at all.
    """
    easy = [i for i in items if i.difficulty == "easy"]
    hard = [i for i in items if i.difficulty == "hard"]
    rng.shuffle(easy)
    rng.shuffle(hard)

    if total_rounds is None:
        total_rounds = len(items)

    # Split the session: the first half from easy, the second from hard. If one
    # pool runs dry the other fills in, so a small or lopsided corpus still
    # produces a full-length session rather than a short one.
    half = total_rounds // 2
    ordered: list[CorpusItem] = []
    ei = hi = 0
    for n in range(total_rounds):
        want_hard = n >= half
        if want_hard and hi < len(hard):
            ordered.append(hard[hi]); hi += 1
        elif not want_hard and ei < len(easy):
            ordered.append(easy[ei]); ei += 1
        elif hi < len(hard):
            ordered.append(hard[hi]); hi += 1
        elif ei < len(easy):
            ordered.append(easy[ei]); ei += 1
        else:
            break  # both pools exhausted

    return _balance_channels(ordered, rng)


def _balance_channels(items: list[CorpusItem], rng) -> list[CorpusItem]:
    """Reorder so the same channel does not appear twice in a row, where possible.

    A greedy pass with a deferred queue: when the next item would repeat the
    previous channel, hold it and try the one after. If nothing else is
    available the repeat is accepted, because refusing to show an item would be
    worse than showing two emails back to back.
    """
    if len(items) < 3:
        return items

    out: list[CorpusItem] = []
    pending: list[CorpusItem] = list(items)

    while pending:
        prev_channel = out[-1].channel if out else None
        pick_index = -1
        for idx, cand in enumerate(pending):
            if cand.channel != prev_channel:
                pick_index = idx
                break
        if pick_index == -1:
            # Every remaining item repeats the previous channel. Take the first
            # rather than stalling forever.
            pick_index = 0
        out.append(pending.pop(pick_index))

    return out


def item_to_payload(item: CorpusItem, *, include_answer: bool) -> dict[str, object]:
    """Serialise an item for transmission to the phone.

    ``include_answer`` is False during play and True only on the review page.
    Sending the label during play would let anyone reading the network traffic
    or the page source cheat, which defeats the entire purpose of the tool.
    """
    payload: dict[str, object] = {
        "id": item.id,
        "channel": item.channel,
        "sender": item.sender,
        "subject": item.subject,
        "body": item.body,
    }
    if include_answer:
        payload["label"] = item.label
        payload["cues"] = list(item.cues)
        payload["explanation"] = item.explanation
        payload["lesson"] = item.lesson
    return payload
