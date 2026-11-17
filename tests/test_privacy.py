"""
Privacy: identity derivation, nickname validation and UID redaction.

These tests are the ones that would catch a regression with real consequences for
the people using the console, so they assert the properties rather than the values:
the same inputs must give the same id, different inputs must not, and the raw UID
must never appear in a log line or a database row.
"""

from __future__ import annotations

import logging

import pytest

from phishtap import privacy
from phishtap.privacy import (
    UidRedactionFilter,
    card_ref,
    identity_bytes,
    install_uid_redaction,
    player_id,
    redact_for_log,
    server_secret,
    validate_nickname,
)

UID = "C7DA4104"


# ── Server secret ─────────────────────────────────────────────────────────

def test_secret_is_32_bytes():
    assert len(server_secret()) == 32


def test_secret_is_stable_within_a_process():
    assert server_secret() == server_secret()


def test_secret_is_persisted_mode_0600(tmp_path):
    path = tmp_path / "secret.key"
    secret = privacy._load_or_create_secret(path)
    assert path.exists()
    assert path.read_bytes() == secret
    mode = path.stat().st_mode & 0o777
    assert mode == 0o600, f"secret file is {oct(mode)}, expected 0600"


def test_secret_is_reloaded_not_regenerated(tmp_path):
    """Regenerating the secret on every boot would orphan every existing
    player_id, silently resetting the leaderboard and all history."""
    path = tmp_path / "secret.key"
    first = privacy._load_or_create_secret(path)
    second = privacy._load_or_create_secret(path)
    assert first == second


# ── Identity ──────────────────────────────────────────────────────────────

def test_player_id_is_deterministic():
    assert player_id(UID, "Ananya") == player_id(UID, "Ananya")


def test_player_id_is_sixteen_hex_chars():
    pid = player_id(UID, "Ananya")
    assert len(pid) == 16
    assert all(c in "0123456789abcdef" for c in pid)


def test_different_cards_give_different_ids():
    assert player_id("AAAAAAAA", "Ananya") != player_id("BBBBBBBB", "Ananya")


def test_shared_card_mode_separates_people_on_one_card():
    """The whole reason shared_card mode folds in the nickname: one demo card is
    used by several students, and without this they would all collapse into a
    single row and overwrite each other's history."""
    a = player_id(UID, "Ananya")
    b = player_id(UID, "Rohit")
    assert a != b


def test_nickname_normalisation_folds_case_and_whitespace():
    """Two spellings a human would consider the same person must map to the same
    identity, or consent would be re-shown and history split."""
    assert player_id(UID, "Ananya") == player_id(UID, "ananya")
    assert player_id(UID, "Ananya") == player_id(UID, "  Ananya  ")
    assert player_id(UID, "ANANYA") == player_id(UID, "ananya")


def test_fullwidth_characters_normalise_to_ascii():
    """NFKC folding means a fullwidth nickname does not create a second identity
    for the same person."""
    assert identity_bytes(UID, "ＡＢＣ") == identity_bytes(UID, "ABC")


def test_separator_prevents_boundary_collisions():
    """Without the 0x00 separator, uid="AB"+nick="CD" and uid="ABC"+nick="D"
    would produce identical identity bytes and merge two different players."""
    a = identity_bytes("AB", "CD")
    b = identity_bytes("ABC", "D")
    assert a != b
    assert b"\x00" in a


def test_per_card_mode_ignores_the_nickname(monkeypatch):
    monkeypatch.setattr(privacy, "IDENTITY_MODE", "per_card")
    assert identity_bytes(UID, "Ananya") == identity_bytes(UID, "Rohit")
    assert identity_bytes(UID, "Ananya") == UID.encode()


def test_shared_card_without_a_nickname_falls_back_to_the_uid():
    """Documented limitation of demo mode rather than a crash: with no nickname
    there is nothing to separate players, so the card alone identifies them."""
    assert identity_bytes(UID, None) == UID.encode()
    assert identity_bytes(UID, "") == UID.encode()


def test_card_ref_differs_from_player_id():
    """card_ref groups by physical card, player_id identifies a person. In
    shared-card mode one card maps to many players, so conflating them would
    collapse exactly the identities the nickname folding keeps apart."""
    assert card_ref(UID) != player_id(UID, "Ananya")
    assert card_ref(UID) != player_id(UID, "Rohit")


def test_card_ref_is_stable_and_uid_specific():
    assert card_ref(UID) == card_ref(UID)
    assert card_ref("AAAAAAAA") != card_ref("BBBBBBBB")
    assert len(card_ref(UID)) == 16


def test_card_ref_does_not_expose_the_uid():
    """The point of card_ref is that the database can group by card without
    storing the identifier. If the UID were recoverable from it, the whole
    exercise would be decoration."""
    ref = card_ref(UID)
    assert UID.lower() not in ref
    assert UID not in ref


# ── Nickname validation ───────────────────────────────────────────────────

@pytest.mark.parametrize(
    "raw",
    ["Ananya", "Rohit", "abc", "Student 1", "priya_s"],
)
def test_valid_nicknames_are_accepted(raw):
    ok, out = validate_nickname(raw)
    assert ok, out


@pytest.mark.parametrize(
    "raw,reason",
    [
        ("", "at least"),
        ("ab", "at least"),
        ("   ", "at least"),
        ("x" * 13, "at most"),
        (None, "required"),
    ],
)
def test_invalid_nicknames_are_rejected(raw, reason):
    ok, out = validate_nickname(raw)
    assert not ok
    assert reason in out


def test_display_name_preserves_case():
    """The identity bytes are case-folded but the display name must not be, or
    every leaderboard entry would be lowercase and students could not find
    themselves on it."""
    ok, display = validate_nickname("  Ananya  ")
    assert ok
    assert display == "Ananya"


@pytest.mark.parametrize("bad", ["fuckhead", "bhosdi", "chutiya", "MrRandi"])
def test_blocked_words_are_rejected(bad):
    ok, out = validate_nickname(bad)
    assert not ok
    assert "blocked" in out


def test_blocked_words_are_caught_regardless_of_case():
    ok, _ = validate_nickname("FUCKHEAD")
    assert not ok


# ── Redaction ─────────────────────────────────────────────────────────────

def test_log_filter_redacts_uid_keys():
    record = logging.LogRecord(
        "t", logging.INFO, __file__, 1, "msg", None, None
    )
    record.uid_hex = UID
    record.uid = UID
    assert UidRedactionFilter().filter(record) is True
    assert record.uid_hex == "[REDACTED]"
    assert record.uid == "[REDACTED]"


def test_log_filter_leaves_other_fields_alone():
    record = logging.LogRecord("t", logging.INFO, __file__, 1, "msg", None, None)
    record.state = "idle"
    UidRedactionFilter().filter(record)
    assert record.state == "idle"


def test_redact_for_log_copies_rather_than_mutates():
    """Mutating the caller's dict would corrupt the event the engine is about to
    process. The redaction is for the log line only."""
    original = {"uid_hex": UID, "seq": 1}
    redacted = redact_for_log(original)
    assert original["uid_hex"] == UID
    assert redacted["uid_hex"] == "[REDACTED]"
    assert redacted["seq"] == 1


def test_install_uid_redaction_is_idempotent():
    install_uid_redaction()
    install_uid_redaction()
    root = logging.getLogger()
    filters = [f for f in root.filters if isinstance(f, UidRedactionFilter)]
    assert len(filters) == 1, "the filter was attached more than once"


def test_no_raw_uid_reaches_a_log_line(caplog):
    """End-to-end: with the filter installed, emitting a record carrying the UID
    as an extra attribute must not put the UID in the output."""
    install_uid_redaction()
    log = logging.getLogger("phishtap.test.redaction")
    with caplog.at_level(logging.INFO, logger="phishtap.test.redaction"):
        log.info("tap %s", "x", extra={"uid_hex": UID})
    assert UID not in caplog.text
