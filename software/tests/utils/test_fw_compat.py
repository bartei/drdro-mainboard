"""Version-coherence rule tests.

One version for the whole stack: the board's firmware must EQUAL the installed software
version. These tests pin the three behaviours the design depends on — equality (not a
minimum) is what's enforced, a mismatch in EITHER direction is a fault, and an unparseable
version never nags.
"""
from dro.utils.fw_compat import (
    AHEAD,
    BEHIND,
    MATCH,
    UNKNOWN,
    compare_versions,
    fw_update_required,
    mismatch_message,
    normalize_version,
    parse_fw_version,
    versions_match,
)


# ── parsing ──────────────────────────────────────────────────────────
def test_parse_release():
    assert parse_fw_version("v1.2.0") == (1, 2, 0, 1, 0)
    assert parse_fw_version("1.2.0") == (1, 2, 0, 1, 0)
    assert parse_fw_version("v1.12.3") == (1, 12, 3, 1, 0)


def test_parse_prerelease():
    assert parse_fw_version("v1.2.0-beta.1") == (1, 2, 0, 0, 1)
    assert parse_fw_version("v1.2.0-rc.2") == (1, 2, 0, 0, 2)


def test_parse_git_describe_suffix_ignored():
    assert parse_fw_version("v1.2.0-3-g1234abc") == (1, 2, 0, 1, 0)
    assert parse_fw_version("v1.2.0-3-g1234abc-dirty") == (1, 2, 0, 1, 0)
    assert parse_fw_version("v1.2.0-beta.1-3-g1234abc") == (1, 2, 0, 0, 1)


def test_parse_garbage_returns_none():
    assert parse_fw_version("unknown") is None
    assert parse_fw_version("") is None
    assert parse_fw_version(None) is None
    assert parse_fw_version("g1234abc") is None      # --always fallback: bare hash


def test_prerelease_sorts_before_its_release():
    assert parse_fw_version("v1.2.0-beta.1") < parse_fw_version("v1.2.0")


# ── normalisation ────────────────────────────────────────────────────
def test_normalize_adds_v_and_strips_describe_suffix():
    assert normalize_version("1.2.0") == "v1.2.0"
    assert normalize_version("v1.2.0") == "v1.2.0"
    assert normalize_version("v1.2.0-3-gabc1234-dirty") == "v1.2.0"
    assert normalize_version("  v1.2.0  ") == "v1.2.0"


def test_normalize_keeps_prerelease():
    assert normalize_version("1.2.0-beta.3") == "v1.2.0-beta.3"


def test_normalize_passes_through_unparseable():
    assert normalize_version("unknown") == "unknown"
    assert normalize_version("") == ""
    assert normalize_version(None) == ""


def test_dev_build_normalizes_equal_to_its_tag():
    # The point of stripping the describe suffix: a dev build of v1.2.0 is v1.2.0.
    assert normalize_version("v1.2.0-5-gdeadbee") == normalize_version("v1.2.0")


# ── comparison ───────────────────────────────────────────────────────
def test_compare_match():
    assert compare_versions("v1.2.0", "v1.2.0") == MATCH
    assert compare_versions("1.2.0", "v1.2.0") == MATCH          # v prefix is cosmetic
    assert compare_versions("v1.2.0-3-gabc", "v1.2.0") == MATCH  # dev build of the same tag


def test_compare_behind_and_ahead():
    assert compare_versions("v1.1.0", "v1.2.0") == BEHIND
    assert compare_versions("v1.3.0", "v1.2.0") == AHEAD
    assert compare_versions("v1.2.0-beta.1", "v1.2.0") == BEHIND
    assert compare_versions("v1.2.0", "v1.2.0-beta.1") == AHEAD


def test_compare_unknown_when_either_side_unparseable():
    assert compare_versions("unknown", "v1.2.0") == UNKNOWN
    assert compare_versions("v1.2.0", "unknown") == UNKNOWN
    assert compare_versions("", "") == UNKNOWN


def test_versions_match_is_strict():
    assert versions_match("v1.2.0", "v1.2.0") is True
    assert versions_match("v1.1.0", "v1.2.0") is False
    # Unknown is explicitly NOT a match — we cannot claim coherence we cannot verify.
    assert versions_match("unknown", "v1.2.0") is False


# ── the rule ─────────────────────────────────────────────────────────
def test_update_required_when_board_is_behind():
    assert fw_update_required("v1.1.0", "v1.2.0") is True
    assert fw_update_required("v1.2.0-beta.1", "v1.2.0") is True


def test_update_required_when_board_is_ahead():
    # The key difference from the old minimum-version gate: newer firmware is also a fault,
    # because the stack is supposed to move as one unit.
    assert fw_update_required("v1.3.0", "v1.2.0") is True
    assert fw_update_required("v2.0.0", "v1.2.0") is True


def test_no_update_required_when_versions_match():
    assert fw_update_required("v1.2.0", "v1.2.0") is False
    assert fw_update_required("v1.2.0-3-g1234abc", "v1.2.0") is False


def test_unparseable_never_nags():
    # A developer running an untagged local build must not be pestered.
    assert fw_update_required("unknown", "v1.2.0") is False
    assert fw_update_required("", "v1.2.0") is False
    assert fw_update_required("v1.2.0", "unknown") is False


# ── messaging ────────────────────────────────────────────────────────
def test_mismatch_message_distinguishes_direction():
    older = mismatch_message("v1.1.0", "v1.2.0")
    newer = mismatch_message("v1.3.0", "v1.2.0")
    assert "older" in older and "v1.1.0" in older and "v1.2.0" in older
    assert "newer" in newer and "v1.3.0" in newer and "v1.2.0" in newer


def test_mismatch_message_empty_when_nothing_to_say():
    assert mismatch_message("v1.2.0", "v1.2.0") == ""
    assert mismatch_message("unknown", "v1.2.0") == ""


def test_mismatch_message_normalizes_displayed_versions():
    msg = mismatch_message("1.1.0-2-gabc1234", "1.2.0")
    assert "v1.1.0" in msg and "v1.2.0" in msg
    assert "gabc1234" not in msg
