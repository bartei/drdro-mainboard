"""Advanced firmware screen logic.

The important property under the single-version design: this page can no longer let a user
pick an arbitrary firmware version off GitHub, because that is exactly how you build the
mismatch the design prevents. It flashes the staged image, or a local file, and nothing else.
"""
import asyncio

import pytest
from kivy.clock import Clock

from dro.comms.stack_update import Paths, stage_firmware
from dro.comms.updater import UpdaterError
from dro.components.screens.firmware_screen import FirmwareScreen


class FakeBoard:
    def __init__(self, connected=True):
        self.connected = connected
        self.connection = object()      # FirmwareUpdater grabs board.connection on build


class FakeApp:
    def __init__(self, connected=True):
        self.board = FakeBoard(connected)


class FakeUpdater:
    def __init__(self, version="v1.2.0", bank=1, ok=True):
        self._version = version
        self._bank = bank
        self.ok = ok
        self.bank_set = None
        self.reset_called = False

    async def get_version(self):
        return self._version

    async def get_active_bank(self):
        return self._bank

    async def set_active_bank(self, bank):
        self.bank_set = bank
        return self.ok

    async def reset(self):
        self.reset_called = True


def _screen(tmp_path, connected=True, updater=None, software="v1.2.0"):
    s = FirmwareScreen.__new__(FirmwareScreen)
    s.app = FakeApp(connected)
    s.paths = Paths(root=tmp_path / "opt")
    s._updater = updater or FakeUpdater()
    s.current_version = "—"
    s.software_version = software
    s.active_bank_text = "—"
    s.boot_bank = ""
    s.progress = 0.0
    s.busy = False
    s.status_text = ""
    s.staged_version = ""
    return s


def flush():
    Clock.tick()


def _stage(tmp_path, paths, version="v1.2.0"):
    src = tmp_path / "fw.bin"
    src.write_bytes(b"FIRMWARE")
    return stage_firmware(src, version, paths)


# ── no release picker ────────────────────────────────────────────────
def test_screen_has_no_release_listing_api():
    # Guard the design decision: an independent firmware version picker must not come back.
    for gone in ("refresh_releases", "select_version", "install_selected", "version_options"):
        assert not hasattr(FirmwareScreen, gone), f"{gone} should not exist any more"


# ── status ───────────────────────────────────────────────────────────
def test_refresh_status_reads_version_and_bank(tmp_path):
    s = _screen(tmp_path)
    asyncio.run(s._refresh_status())
    assert s.current_version == "v1.2.0"
    assert s.active_bank_text == "1"
    assert s.boot_bank == "1"


def test_refresh_status_offline(tmp_path):
    s = _screen(tmp_path, connected=False)
    asyncio.run(s._refresh_status())
    assert s.current_version == "(offline)"
    assert s.active_bank_text == "—"


def test_refresh_status_tolerates_missing_values(tmp_path):
    s = _screen(tmp_path, updater=FakeUpdater(version=None, bank=None))
    asyncio.run(s._refresh_status())
    assert s.current_version == "—"
    assert s.active_bank_text == "—"


# ── staged image ─────────────────────────────────────────────────────
def test_refresh_staged_finds_a_pending_image(tmp_path):
    s = _screen(tmp_path)
    _stage(tmp_path, s.paths)
    s._refresh_staged()
    assert s.staged_version == "v1.2.0"


def test_refresh_staged_empty_when_none(tmp_path):
    s = _screen(tmp_path)
    s._refresh_staged()
    assert s.staged_version == ""


def test_refresh_staged_ignores_a_stale_marker(tmp_path):
    s = _screen(tmp_path, software="v1.1.0")
    _stage(tmp_path, s.paths, version="v1.2.0")
    s._refresh_staged()
    assert s.staged_version == ""


def test_flash_staged_without_an_image_reports_it(tmp_path):
    s = _screen(tmp_path)
    spawned = []
    s._spawn = spawned.append
    s.flash_staged()
    flush()
    assert "No staged firmware" in s.status_text
    assert spawned == []


def test_flash_staged_spawns_the_flash(tmp_path):
    s = _screen(tmp_path)
    _stage(tmp_path, s.paths)
    spawned = []
    s._spawn = lambda c: (spawned.append(c), c.close())
    s.flash_staged()
    assert len(spawned) == 1


def test_flash_staged_is_a_noop_while_busy(tmp_path):
    s = _screen(tmp_path)
    _stage(tmp_path, s.paths)
    s.busy = True
    spawned = []
    s._spawn = spawned.append
    s.flash_staged()
    assert spawned == []


# ── manual flash ─────────────────────────────────────────────────────
def test_flash_path_rejects_a_missing_file(tmp_path):
    s = _screen(tmp_path)
    spawned = []
    s._spawn = spawned.append
    s.flash_path(str(tmp_path / "nope.bin"))
    flush()
    assert "No such file" in s.status_text
    assert spawned == []


def test_flash_path_rejects_empty_input(tmp_path):
    s = _screen(tmp_path)
    spawned = []
    s._spawn = spawned.append
    s.flash_path("")
    flush()
    assert "No such file" in s.status_text


def test_flash_path_accepts_an_existing_file(tmp_path):
    s = _screen(tmp_path)
    f = tmp_path / "local.bin"
    f.write_bytes(b"X")
    spawned = []
    s._spawn = lambda c: (spawned.append(c), c.close())
    s.flash_path(str(f))
    assert len(spawned) == 1


def test_flash_path_is_a_noop_while_busy(tmp_path):
    s = _screen(tmp_path)
    f = tmp_path / "local.bin"
    f.write_bytes(b"X")
    s.busy = True
    spawned = []
    s._spawn = spawned.append
    s.flash_path(str(f))
    assert spawned == []


# ── the flash coroutine ──────────────────────────────────────────────
def test_flash_success_clears_busy_and_refreshes(tmp_path, monkeypatch):
    s = _screen(tmp_path)
    _stage(tmp_path, s.paths)
    called = {}

    async def fake_apply(board, path, version="", on_status=None, on_progress=None):
        called["path"] = path
        if on_status:
            on_status("flashing")
        return {"bank": 1}

    monkeypatch.setattr(
        "dro.components.screens.firmware_screen.apply_firmware", fake_apply)

    asyncio.run(s._flash(str(s.paths.staged_firmware), "v1.2.0", clear_staged=True))
    flush()
    assert called["path"].endswith("staged-firmware.bin")
    assert s.busy is False
    # A staged image flashed by hand is spent — marker and image both go.
    assert s.staged_version == ""
    assert not s.paths.marker.exists()


def test_manual_file_flash_leaves_any_staged_image_alone(tmp_path, monkeypatch):
    # Flashing an arbitrary recovery .bin must not discard a pending staged update.
    s = _screen(tmp_path)
    _stage(tmp_path, s.paths)

    async def fake_apply(*a, **kw):
        return {"bank": 0}

    monkeypatch.setattr(
        "dro.components.screens.firmware_screen.apply_firmware", fake_apply)

    asyncio.run(s._flash("/tmp/recovery.bin", ""))
    flush()
    assert s.paths.marker.exists()
    assert s.staged_version == "v1.2.0"


def test_flash_failure_is_reported_and_clears_busy(tmp_path, monkeypatch):
    s = _screen(tmp_path)

    async def boom(*a, **kw):
        raise UpdaterError("bootloader silent")

    monkeypatch.setattr("dro.components.screens.firmware_screen.apply_firmware", boom)

    asyncio.run(s._flash("/tmp/x.bin", "v1.2.0"))
    flush()
    assert "Flash FAILED" in s.status_text
    assert s.busy is False


def test_flash_os_error_is_reported(tmp_path, monkeypatch):
    s = _screen(tmp_path)

    async def boom(*a, **kw):
        raise OSError("serial gone")

    monkeypatch.setattr("dro.components.screens.firmware_screen.apply_firmware", boom)
    asyncio.run(s._flash("/tmp/x.bin", ""))
    flush()
    assert "Flash FAILED" in s.status_text


# ── bank control ─────────────────────────────────────────────────────
@pytest.mark.parametrize("bad", ["2", "", "x", None])
def test_set_bank_rejects_invalid_values(tmp_path, bad):
    s = _screen(tmp_path)
    spawned = []
    s._spawn = spawned.append
    s.set_bank(bad)
    assert spawned == []


def test_set_bank_skips_when_already_active(tmp_path):
    s = _screen(tmp_path)
    s.active_bank_text = "1"
    spawned = []
    s._spawn = spawned.append
    s.set_bank("1")
    assert s.boot_bank == "1"
    assert spawned == []                       # no round-trip for a no-op


def test_set_bank_issues_the_change(tmp_path):
    s = _screen(tmp_path)
    s.active_bank_text = "0"
    updater = s._updater
    asyncio.run(s._set_bank(1))
    flush()
    assert updater.bank_set == 1
    assert "Bank set" in s.status_text


def test_set_bank_reports_failure(tmp_path):
    s = _screen(tmp_path, updater=FakeUpdater(ok=False))
    asyncio.run(s._set_bank(1))
    flush()
    assert "Bank set failed" in s.status_text


def test_reset_reboots_the_firmware(tmp_path, monkeypatch):
    s = _screen(tmp_path)
    real_sleep = asyncio.sleep                    # capture before patching, or it recurses
    monkeypatch.setattr(asyncio, "sleep", lambda *_a, **_kw: real_sleep(0))
    asyncio.run(s._reset())
    flush()
    assert s._updater.reset_called is True
    assert "rebooted" in s.status_text


def test_status_log_is_bounded(tmp_path):
    s = _screen(tmp_path)
    for i in range(500):
        s._status("x" * 50)
        flush()
    assert len(s.status_text) <= 4000


# ── lifecycle / delegation ───────────────────────────────────────────
# NOTE: Screen.__init__ requires a real Window, unavailable under the headless mock-GL
# backend, so these drive a __new__-built instance like the rest of the suite. __init__
# itself is pure wiring and is marked no-cover in the module.
def test_updater_property_is_lazy_and_cached(tmp_path):
    s = _screen(tmp_path)
    s._updater = None
    first = s.updater
    assert first is not None
    assert s.updater is first                   # built once, reused


def test_on_pre_enter_refreshes_everything(tmp_path, monkeypatch):
    s = _screen(tmp_path)
    _stage(tmp_path, s.paths, version="v1.2.0")
    calls = []
    s.refresh_status = lambda: calls.append("status")
    monkeypatch.setattr("dro.components.screens.firmware_screen.installed_version",
                        lambda: "v1.2.0")

    s.on_pre_enter()

    assert calls == ["status"]
    assert s.staged_version == "v1.2.0"         # the staged image was picked up


def test_refresh_status_spawns(tmp_path):
    s = _screen(tmp_path)
    spawned = []
    s._spawn = lambda c: (spawned.append(c), c.close())
    s.refresh_status()
    assert len(spawned) == 1


def test_set_bank_spawns_a_change(tmp_path):
    s = _screen(tmp_path)
    s.active_bank_text = "0"
    spawned = []
    s._spawn = lambda c: (spawned.append(c), c.close())
    s.set_bank("1")
    assert s.boot_bank == "1"
    assert len(spawned) == 1


def test_do_reset_spawns(tmp_path):
    s = _screen(tmp_path)
    spawned = []
    s._spawn = lambda c: (spawned.append(c), c.close())
    s.do_reset()
    flush()
    assert len(spawned) == 1
    assert "Rebooting" in s.status_text


def test_spawn_schedules_on_a_running_loop(tmp_path):
    s = _screen(tmp_path)
    ran = []

    async def work():
        ran.append(True)

    async def drive():
        s._spawn(work())
        await asyncio.sleep(0)

    asyncio.run(drive())
    assert ran == [True]
