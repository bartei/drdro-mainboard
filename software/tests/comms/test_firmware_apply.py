"""Tests for the shared flash-with-settings-preservation path.

Both the automatic post-reboot flash and the manual advanced-screen flash go through
apply_firmware, so the ordering pinned here (back up -> flash -> wait -> restore) applies
to both and cannot drift between them.
"""
import asyncio

import pytest

from dro.comms.firmware_apply import apply_firmware
from dro.comms.updater import UpdaterError


class FakeBoard:
    def __init__(self, connected=True):
        self.connected = connected


class FakeProfiles:
    def __init__(self, snapshot=("snap",), restored=2, fail=None):
        self._snapshot = snapshot
        self._restored = restored
        self.fail = fail or set()
        self.calls = []

    async def snapshot_board(self):
        self.calls.append("snapshot")
        if "snapshot" in self.fail:
            raise OSError("cannot read board")
        return self._snapshot

    async def backup_current(self, note=""):
        self.calls.append(f"backup:{note}")
        if "backup" in self.fail:
            raise OSError("disk full")
        from pathlib import Path
        return Path("/profiles/backup-2026.yaml")

    async def restore_board_settings(self, snapshot):
        self.calls.append("restore")
        if "restore" in self.fail:
            raise OSError("write failed")
        return self._restored


class FakeUpdater:
    def __init__(self, result=None, fail=False):
        self.result = result or {"bank": 1, "size": 1024, "crc": "AB", "version": "v1.2.0"}
        self.fail = fail
        self.installed = None

    async def install(self, bin_path, on_progress=None, on_status=None):
        self.installed = bin_path
        if self.fail:
            raise UpdaterError("flash failed: bank write error")
        if on_progress:
            on_progress(0.5)
        if on_status:
            on_status("flashing")
        return self.result


def run(**kw):
    board = kw.pop("board", None) or FakeBoard()
    profiles = kw.pop("profiles", None) or FakeProfiles()
    updater = kw.pop("updater", None) or FakeUpdater()
    statuses = []
    result = asyncio.run(apply_firmware(
        board, "/tmp/fw.bin", version=kw.pop("version", "v1.2.0"),
        on_status=statuses.append, updater=updater, profiles=profiles, **kw,
    ))
    return result, statuses, profiles, updater


def test_backs_up_before_flashing_and_restores_after():
    _, _, profiles, updater = run()
    assert updater.installed == "/tmp/fw.bin"
    # Ordering is the contract: both backups precede the flash, restore follows it.
    assert profiles.calls[0] == "snapshot"
    assert profiles.calls[1].startswith("backup:")
    assert profiles.calls[-1] == "restore"


def test_backup_note_names_the_target_version():
    _, _, profiles, _ = run(version="v9.9.9")
    assert any("v9.9.9" in c for c in profiles.calls)


def test_falls_back_to_generic_label_without_a_version():
    _, _, profiles, _ = run(version="")
    assert any("new firmware" in c for c in profiles.calls)


def test_returns_the_updater_result():
    result, _, _, _ = run()
    assert result["bank"] == 1 and result["version"] == "v1.2.0"


def test_reports_restored_setting_count():
    _, statuses, _, _ = run(profiles=FakeProfiles(restored=3))
    assert any("Restored 3" in s for s in statuses)


def test_reports_when_nothing_was_lost():
    _, statuses, _, _ = run(profiles=FakeProfiles(restored=0))
    assert any("nothing lost" in s for s in statuses)


def test_backup_failure_does_not_block_the_flash():
    # The board keeps its own settings in flash and dual-bank rollback is the real safety
    # net — a failed backup must not strand the user on old firmware.
    _, statuses, _, updater = run(profiles=FakeProfiles(fail={"backup"}))
    assert updater.installed == "/tmp/fw.bin"
    assert any("Could not back up" in s for s in statuses)


def test_snapshot_failure_skips_restore_but_still_flashes():
    _, _, profiles, updater = run(profiles=FakeProfiles(fail={"snapshot"}))
    assert updater.installed == "/tmp/fw.bin"
    assert "restore" not in profiles.calls


def test_restore_failure_is_reported_not_raised():
    _, statuses, _, _ = run(profiles=FakeProfiles(fail={"restore"}))
    assert any("Could not verify board settings" in s for s in statuses)


def test_flash_failure_propagates():
    # The caller decides whether this is fatal — the board is still bootable via rollback.
    with pytest.raises(UpdaterError, match="flash failed"):
        run(updater=FakeUpdater(fail=True))


def test_skips_restore_when_board_never_reconnects(monkeypatch):
    monkeypatch.setattr("dro.comms.firmware_apply.RECONNECT_TRIES", 2)
    monkeypatch.setattr("dro.comms.firmware_apply.RECONNECT_DELAY", 0)
    profiles = FakeProfiles()
    _, statuses, _, _ = run(board=FakeBoard(connected=False), profiles=profiles)
    assert "restore" not in profiles.calls
    assert any("Waiting for the board" in s for s in statuses)


def test_works_without_callbacks():
    # The default no-op callbacks must not blow up.
    board, profiles, updater = FakeBoard(), FakeProfiles(), FakeUpdater()
    result = asyncio.run(apply_firmware(board, "/tmp/fw.bin",
                                        updater=updater, profiles=profiles))
    assert result["bank"] == 1


def test_final_status_summarises_the_result():
    _, statuses, _, _ = run()
    assert any("Firmware updated" in s and "bank 1" in s for s in statuses)
