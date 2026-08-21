"""Board version-coherence wiring.

Exercises the real `_refresh_version_coherence` against a bare Board (no link, no Kivy
app), matching the rest of the suite's style.
"""
from dro.dispatchers.board import Board


def _board(firmware, software):
    b = Board.__new__(Board)
    b.firmware_version = firmware
    b.software_version = software
    return b


def test_match_clears_the_flag_and_message():
    b = _board("v1.2.0", "v1.2.0")
    b._refresh_version_coherence()
    assert b.firmware_update_required is False
    assert b.firmware_mismatch == ""


def test_board_behind_raises_the_flag():
    b = _board("v1.1.0", "v1.2.0")
    b._refresh_version_coherence()
    assert b.firmware_update_required is True
    assert "older" in b.firmware_mismatch


def test_board_ahead_also_raises_the_flag():
    # Single-version design: newer firmware is just as incoherent as older.
    b = _board("v1.3.0", "v1.2.0")
    b._refresh_version_coherence()
    assert b.firmware_update_required is True
    assert "newer" in b.firmware_mismatch


def test_dev_build_of_the_same_tag_is_coherent():
    b = _board("v1.2.0-4-gabc1234", "v1.2.0")
    b._refresh_version_coherence()
    assert b.firmware_update_required is False


def test_unknown_versions_stay_quiet():
    b = _board("unknown", "v1.2.0")
    b._refresh_version_coherence()
    assert b.firmware_update_required is False
    assert b.firmware_mismatch == ""


def test_empty_firmware_version_stays_quiet():
    # Before the first successful `version` read the board reports "".
    b = _board("", "v1.2.0")
    b._refresh_version_coherence()
    assert b.firmware_update_required is False


def test_unknown_software_version_stays_quiet():
    # A source checkout has no package metadata — do not nag a developer.
    b = _board("v1.2.0", "unknown")
    b._refresh_version_coherence()
    assert b.firmware_update_required is False


def test_recheck_clears_a_previously_raised_flag():
    b = _board("v1.1.0", "v1.2.0")
    b._refresh_version_coherence()
    assert b.firmware_update_required is True

    b.firmware_version = "v1.2.0"          # after a successful flash
    b._refresh_version_coherence()
    assert b.firmware_update_required is False
    assert b.firmware_mismatch == ""
