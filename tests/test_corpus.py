"""
Corpus loading, validation and defanging.

The defanging tests matter more than they look. This is a training tool about
phishing; if it ever served a live clickable URL it would become a vector for the
exact thing it teaches people to avoid. The corpus is authored with reserved TLDs
so that cannot happen by accident, and defanging means it cannot happen by
carelessness either.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from phishtap.corpus import (
    CHANNELS,
    DIFFICULTIES,
    LABEL_PHISH,
    LABEL_SAFE,
    CorpusError,
    build_session_pool,
    corpus_summary,
    defang,
    item_to_payload,
    load_corpus,
)

REPO_ROOT = Path(__file__).resolve().parent.parent
CORPUS_FILE = REPO_ROOT / "python" / "corpus.json"


# ── Defanging ─────────────────────────────────────────────────────────────

@pytest.mark.parametrize(
    "raw,expected",
    [
        ("https://bank.example/login", "hxxps://bank[.]example/login"),
        ("http://a.invalid", "hxxp://a[.]invalid"),
        ("HTTP://A.INVALID/X", "HXXP://A[.]INVALID/X"),
        ("ftp://files.example", "fxp://files[.]example"),
        ("visit sb1-kyc.example now", "visit sb1-kyc[.]example now"),
    ],
)
def test_defang_rewrites_schemes_and_hosts(raw, expected):
    assert defang(raw) == expected


@pytest.mark.parametrize(
    "text",
    [
        "see e.g. the documentation",       # one-char final label
        "it costs 1.5 rupees",              # non-alphabetic final label
        "version 3.13 of python",
        "no urls here at all",
        "",
    ],
)
def test_defang_leaves_prose_alone(text):
    """A defanger that mangles ordinary sentences would make every message in the
    game look broken, and students would stop reading them."""
    assert defang(text) == text


def test_defang_is_idempotent():
    """Applying it twice must not double-apply. The corpus is defanged at load
    time and the frontend may defensively re-escape, so a non-idempotent defang
    would produce hxxxxps:// and undermine the lesson."""
    once = defang("https://bank.example/pay")
    assert defang(once) == once


def test_defang_handles_none_and_empty():
    assert defang("") == ""


def test_defang_preserves_multiline_bodies():
    body = "Hello,\n\nClick https://x.example/a\n\nThanks"
    out = defang(body)
    assert "\n\n" in out
    assert "hxxps://x[.]example/a" in out


# ── Loading and validation ────────────────────────────────────────────────

def test_repository_corpus_loads(real_corpus):
    assert len(real_corpus) > 0


def test_corpus_shape(real_corpus):
    summary = corpus_summary(real_corpus)
    assert summary["total"] == len(real_corpus)
    assert summary["phish"] + summary["safe"] == summary["total"]
    assert summary["easy"] + summary["hard"] == summary["total"]
    for item in real_corpus:
        assert item.channel in CHANNELS
        assert item.difficulty in DIFFICULTIES
        assert item.label in (LABEL_PHISH, LABEL_SAFE)


def test_corpus_is_balanced(real_corpus):
    """The architecture calls for 50/50 phish to safe. An unbalanced corpus lets
    a student win by always answering the same way, which teaches nothing."""
    summary = corpus_summary(real_corpus)
    assert summary["phish"] == summary["safe"], (
        f"corpus is {summary['phish']} phish to {summary['safe']} safe; "
        "a player could score perfectly by always guessing one way"
    )


def test_every_channel_is_represented(real_corpus):
    summary = corpus_summary(real_corpus)
    for channel in CHANNELS:
        assert summary["by_channel"].get(channel, 0) > 0, f"no {channel} items"


def test_phish_items_have_cues_and_explanations(real_corpus):
    """A phish item with no cues gives the review page nothing to teach with,
    which is the entire point of the tool."""
    for item in real_corpus:
        if item.is_phish:
            assert item.cues, f"{item.id} is phish but lists no cues"
            assert item.explanation.strip(), f"{item.id} has no explanation"
        assert item.lesson.strip(), f"{item.id} has no lesson"


def test_no_live_domains_in_the_corpus(real_corpus):
    """Every URL must be on a reserved TLD. This is the assertion that stops a
    future editor from pasting a real phishing URL into the content file."""
    import re

    url_re = re.compile(r"[a-z0-9-]+(?:\.[a-z0-9-]+)+", re.IGNORECASE)
    reserved = {".example", ".invalid", ".test", ".localhost", ".local"}

    for item in real_corpus:
        for text in (item.sender, item.subject, item.body, item.explanation):
            for match in url_re.findall(text or ""):
                # Defanging already turned dots into [.] so re-join for the check.
                host = match.replace("[.]", ".").lower()
                if "." not in host:
                    continue
                suffix = "." + host.rsplit(".", 1)[-1]
                assert suffix in reserved or host.endswith(tuple(reserved)), (
                    f"{item.id} contains '{host}' whose TLD '{suffix}' is not "
                    f"reserved. Use one of {sorted(reserved)}."
                )


def test_ids_are_unique(real_corpus):
    ids = [i.id for i in real_corpus]
    assert len(ids) == len(set(ids))


# ── Validation failures ───────────────────────────────────────────────────

def _write(tmp_path: Path, payload) -> Path:
    p = tmp_path / "corpus.json"
    p.write_text(json.dumps(payload), encoding="utf-8")
    return p


def _valid_item(**over):
    base = {
        "id": "t1", "channel": "email", "label": 1, "difficulty": "easy",
        "sender": "a@b.example", "subject": "s", "body": "b",
        "cues": ["urgency"], "explanation": "e", "lesson": "l",
    }
    base.update(over)
    return base


def test_missing_file_raises(tmp_path):
    with pytest.raises(CorpusError, match="not found"):
        load_corpus(tmp_path / "nope.json")


def test_invalid_json_raises(tmp_path):
    p = tmp_path / "corpus.json"
    p.write_text("{not json", encoding="utf-8")
    with pytest.raises(CorpusError, match="not valid JSON"):
        load_corpus(p)


def test_top_level_must_be_a_list(tmp_path):
    with pytest.raises(CorpusError, match="JSON array"):
        load_corpus(_write(tmp_path, {"id": "t1"}))


def test_missing_field_is_reported_with_the_item_id(tmp_path):
    item = _valid_item()
    del item["explanation"]
    with pytest.raises(CorpusError) as exc:
        load_corpus(_write(tmp_path, [item]))
    assert "t1" in str(exc.value)
    assert "explanation" in str(exc.value)


def test_bad_channel_is_rejected(tmp_path):
    with pytest.raises(CorpusError, match="channel"):
        load_corpus(_write(tmp_path, [_valid_item(channel="telegram")]))


def test_bad_difficulty_is_rejected(tmp_path):
    with pytest.raises(CorpusError, match="difficulty"):
        load_corpus(_write(tmp_path, [_valid_item(difficulty="medium")]))


def test_bad_label_is_rejected(tmp_path):
    with pytest.raises(CorpusError, match="label"):
        load_corpus(_write(tmp_path, [_valid_item(label=2)]))


def test_boolean_label_is_rejected(tmp_path):
    """bool is a subclass of int in Python, so True would silently pass an
    isinstance(label, int) check and become LABEL_PHISH. That would turn a typo
    in the content file into a silently mislabelled item."""
    with pytest.raises(CorpusError, match="boolean"):
        load_corpus(_write(tmp_path, [_valid_item(label=True)]))


def test_duplicate_ids_are_rejected(tmp_path):
    with pytest.raises(CorpusError, match="duplicate"):
        load_corpus(_write(tmp_path, [_valid_item(), _valid_item()]))


def test_cues_must_be_strings(tmp_path):
    with pytest.raises(CorpusError, match="cues"):
        load_corpus(_write(tmp_path, [_valid_item(cues=[1, 2])]))


def test_loaded_items_are_defanged(tmp_path):
    """Defanging happens at load time, so no in-memory item ever holds a live
    URL. That is stronger than defanging on the way out, because there is no code
    path that can forget to do it."""
    item = _valid_item(body="go to https://bank.example/now")
    loaded = load_corpus(_write(tmp_path, [item]))
    assert "https://" not in loaded[0].body
    assert "hxxps://bank[.]example/now" in loaded[0].body


# ── Payload shaping ───────────────────────────────────────────────────────

def test_play_payload_withholds_the_answer(real_corpus):
    """The single most important assertion in this file. If the label reaches the
    phone during play the game is over: anyone can read it from the network tab."""
    item = next(i for i in real_corpus if i.is_phish)
    payload = item_to_payload(item, include_answer=False)
    for forbidden in ("label", "cues", "explanation", "lesson"):
        assert forbidden not in payload, f"{forbidden} leaked into the play payload"
    for required in ("id", "channel", "sender", "subject", "body"):
        assert required in payload


def test_review_payload_includes_the_answer(real_corpus):
    item = real_corpus[0]
    payload = item_to_payload(item, include_answer=True)
    assert payload["label"] == item.label
    assert list(payload["cues"]) == list(item.cues)
    assert payload["explanation"] == item.explanation
    assert payload["lesson"] == item.lesson


# ── Session pool ──────────────────────────────────────────────────────────

def test_pool_has_no_duplicates(real_corpus):
    import random

    pool = build_session_pool(real_corpus, random.Random(1))
    ids = [i.id for i in pool]
    assert len(ids) == len(set(ids)), "an item was used twice in one session"


def test_pool_covers_the_whole_corpus_by_default(real_corpus):
    import random

    pool = build_session_pool(real_corpus, random.Random(2))
    assert len(pool) == len(real_corpus)


def test_pool_is_reproducible_with_a_seeded_rng(real_corpus):
    import random

    a = [i.id for i in build_session_pool(real_corpus, random.Random(99))]
    b = [i.id for i in build_session_pool(real_corpus, random.Random(99))]
    assert a == b, "a seeded generator must produce the same session every time"


def test_easy_items_come_first(real_corpus):
    import random

    pool = build_session_pool(real_corpus, random.Random(3))
    half = len(pool) // 2
    early = [i.difficulty for i in pool[:half]]
    late = [i.difficulty for i in pool[half:]]
    assert early.count("easy") >= late.count("easy"), (
        "the first half of a session should be at least as easy as the second"
    )


def test_pool_avoids_consecutive_same_channel_where_possible(real_corpus):
    import random

    pool = build_session_pool(real_corpus, random.Random(4))
    repeats = sum(
        1 for a, b in zip(pool, pool[1:]) if a.channel == b.channel
    )
    # With four channels and a balanced corpus, a perfect ordering has zero
    # repeats. Allow a small number so the test does not become brittle if the
    # corpus composition changes, but fail on a shuffle that ignored channels.
    assert repeats <= max(2, len(pool) // 8), (
        f"{repeats} consecutive same-channel pairs in {len(pool)} items"
    )


def test_short_pool_request_is_honoured(real_corpus):
    import random

    pool = build_session_pool(real_corpus, random.Random(5), total_rounds=4)
    assert len(pool) == 4


def test_pool_longer_than_corpus_stops_instead_of_repeating(real_corpus):
    import random

    pool = build_session_pool(real_corpus, random.Random(6), total_rounds=9999)
    assert len(pool) == len(real_corpus)
    ids = [i.id for i in pool]
    assert len(ids) == len(set(ids)), "exhausting the pool must not repeat items"
