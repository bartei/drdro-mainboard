"""Tests for the post-reboot half of the single-step update.

Retry semantics are the point: the marker is cleared only on success or when it is stale,
so a failed flash retries on the next boot instead of being silently forgotten.
"""
import asyncio

import pytest

from dro.comms.pending_update import PendingUpdateController
from dro.comms.stack_update import Paths, stage_firmware
from dro.comms.updater import UpdaterError


class FakeBoard:
    def __init__(self, connected=True, firmware_version="v1.1.0"):
        self.connected = connected
        self.firmware_version = firmware_version
        self.refreshed = False

    def _refresh_version_coherence(self):
        self.refreshed = True


@pytest.fixture
def paths(tmp_path):
    return Paths(root=tmp_path / "opt-drdro")


@pytest.fixture
def staged(tmp_path, paths):
    src = tmp_path / "fw.bin"
    src.write_bytes(b"FIRMWARE")
    stage_firmware(src, "v1.2.0", paths)
    return paths


def make(board, paths, apply_fn=None, version="v1.2.0"):
    statuses = []
    ctl = PendingUpdateController(
        board, paths=paths, software_version=version,
        on_status=statuses.append, apply_fn=apply_fn,
    )
    return ctl, statuses


def ok_apply(calls):
    async def _apply(board, path, version="", on_status=None, on_progress=None):
        calls.append((path, version))
        board.firmware_version = version
        return {"bank": 1}
    return _apply


# ── nothing to do ────────────────────────────────────────────────────
def test_no_marker_is_a_noop(paths):
    ctl, _ = make(FakeBoard(), paths)
    assert asyncio.run(ctl.run()) is False


def test_stale_marker_is_discarded_without_flashing(staged):
    # Marker says v1.2.0 but the software is on v1.1.0 — flashing would push the board to
    # a version the software isn't on.
    calls = []
    ctl, _ = make(FakeBoard(), staged, apply_fn=ok_apply(calls), version="v1.1.0")

    assert asyncio.run(ctl.run()) is False
    assert calls == []
    assert not staged.marker.exists()


# ── the happy path ───────────────────────────────────────────────────
def test_flashes_staged_image_and_clears_marker(staged):
    calls = []
    board = FakeBoard(firmware_version="v1.1.0")
    ctl, statuses = make(board, staged, apply_fn=ok_apply(calls))

    assert asyncio.run(ctl.run()) is True
    assert calls == [(str(staged.staged_firmware), "v1.2.0")]
    assert not staged.marker.exists()
    assert not staged.staged_firmware.exists()
    assert board.refreshed is True
    assert any("complete" in s for s in statuses)


def test_uses_the_local_image_not_the_network(staged):
    # The whole reason the image is downloaded before the reboot.
    seen = []
    ctl, _ = make(FakeBoard(), staged, apply_fn=ok_apply(seen))
    asyncio.run(ctl.run())
    assert seen[0][0].endswith("staged-firmware.bin")


# ── already-applied ──────────────────────────────────────────────────
def test_board_already_on_target_clears_without_flashing(staged):
    # A retry after the flash succeeded but the marker survived (power cut at the wrong
    # moment) must not reflash.
    calls = []
    board = FakeBoard(firmware_version="v1.2.0")
    ctl, _ = make(board, staged, apply_fn=ok_apply(calls))

    assert asyncio.run(ctl.run()) is False
    assert calls == []
    assert not staged.marker.exists()


def test_dev_build_of_target_counts_as_already_applied(staged):
    calls = []
    board = FakeBoard(firmware_version="v1.2.0-4-gabc1234")
    ctl, _ = make(board, staged, apply_fn=ok_apply(calls))
    assert asyncio.run(ctl.run()) is False
    assert calls == []


# ── failure / retry ──────────────────────────────────────────────────
def test_failed_flash_keeps_marker_for_retry(staged):
    async def boom(board, path, version="", on_status=None, on_progress=None):
        raise UpdaterError("bootloader did not answer")

    ctl, statuses = make(FakeBoard(), staged, apply_fn=boom)

    assert asyncio.run(ctl.run()) is False
    # Marker and image survive so the next boot tries again.
    assert staged.marker.exists()
    assert staged.staged_firmware.exists()
    assert any("will retry" in s for s in statuses)


def test_os_error_during_flash_also_retries(staged):
    async def boom(board, path, version="", on_status=None, on_progress=None):
        raise OSError("serial port vanished")

    ctl, _ = make(FakeBoard(), staged, apply_fn=boom)
    assert asyncio.run(ctl.run()) is False
    assert staged.marker.exists()


def test_board_never_connects_keeps_marker(staged, monkeypatch):
    monkeypatch.setattr("dro.comms.pending_update.CONNECT_TIMEOUT", 0.05)
    monkeypatch.setattr("dro.comms.pending_update.CONNECT_POLL", 0.01)
    calls = []
    ctl, statuses = make(FakeBoard(connected=False), staged, apply_fn=ok_apply(calls))

    assert asyncio.run(ctl.run()) is False
    assert calls == []
    assert staged.marker.exists()
    assert any("not reachable" in s for s in statuses)


def test_waits_for_a_board_that_appears_late(staged, monkeypatch):
    monkeypatch.setattr("dro.comms.pending_update.CONNECT_TIMEOUT", 1.0)
    monkeypatch.setattr("dro.comms.pending_update.CONNECT_POLL", 0.01)
    board = FakeBoard(connected=False)
    calls = []

    async def apply_and_record(b, path, version="", on_status=None, on_progress=None):
        calls.append(path)
        b.firmware_version = version
        return {}

    ctl, _ = make(board, staged, apply_fn=apply_and_record)

    async def drive():
        async def flip():
            await asyncio.sleep(0.03)
            board.connected = True
        await asyncio.gather(ctl.run(), flip())

    asyncio.run(drive())
    assert calls, "controller should flash once the board turns up"


def test_board_without_refresh_hook_is_tolerated(staged):
    class Bare:
        connected = True
        firmware_version = "v1.1.0"

    calls = []
    ctl, _ = make(Bare(), staged, apply_fn=ok_apply(calls))
    assert asyncio.run(ctl.run()) is True
