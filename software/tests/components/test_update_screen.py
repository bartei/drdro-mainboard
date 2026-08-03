"""Update screen logic — the user-facing single-step page.

Built with ``__new__`` and driven directly, like the rest of the suite: no Kivy app, no GL.
``@mainthread`` setters defer through the Clock, so ``Clock.tick()`` flushes them.

What matters here is the page's *decisions*: when Update is offered, what happens when a
check or an update fails, and that a reboot only ever follows a successful install.
"""
import asyncio

from kivy.clock import Clock

from dro.comms.release import ReleaseError
from dro.comms.stack_update import Paths, StackUpdateError
from dro.components.screens.update_screen import UpdateScreen


class FakeRelease:
    def __init__(self, version="v1.3.0"):
        self.version = version
        self.tag = version


class FakeReleases:
    def __init__(self, latest=None, error=None):
        self._latest = latest
        self._error = error
        self.asked_prerelease = None

    async def latest(self, include_prerelease=False):
        self.asked_prerelease = include_prerelease
        if self._error:
            raise self._error
        return self._latest


def _screen(installed="v1.2.0", releases=None, tmp_path=None):
    s = UpdateScreen.__new__(UpdateScreen)
    s.releases = releases or FakeReleases()
    s.paths = Paths(root=(tmp_path / "opt") if tmp_path else Paths().root)
    s._latest = None
    s._reboot = lambda: s.__dict__.setdefault("_rebooted", True)
    s.installed_version = installed
    s.available_version = ""
    s.status = ""
    s.progress = 0.0
    s.busy = False
    s.update_available = False
    s.allow_prerelease = False
    return s


def flush():
    """Run the Clock once so @mainthread callbacks land."""
    Clock.tick()


# ── availability decision ────────────────────────────────────────────
def test_newer_release_offers_an_update():
    s = _screen(installed="v1.2.0", releases=FakeReleases(latest=FakeRelease("v1.3.0")))
    asyncio.run(s._check_for_updates())
    flush()
    assert s.available_version == "v1.3.0"
    assert s.update_available is True
    assert "available" in s.status


def test_same_version_reports_up_to_date():
    s = _screen(installed="v1.2.0", releases=FakeReleases(latest=FakeRelease("v1.2.0")))
    asyncio.run(s._check_for_updates())
    flush()
    assert s.update_available is False
    assert s.status == "Up to date"


def test_dev_build_of_the_same_tag_is_up_to_date():
    # Running a local build of v1.2.0 must not present v1.2.0 as an update.
    s = _screen(installed="v1.2.0-4-gabc1234",
                releases=FakeReleases(latest=FakeRelease("v1.2.0")))
    asyncio.run(s._check_for_updates())
    flush()
    assert s.update_available is False


def test_older_release_still_counts_as_a_difference():
    # A downgrade is still an available action — coherence is equality, not ordering.
    s = _screen(installed="v1.3.0", releases=FakeReleases(latest=FakeRelease("v1.2.0")))
    asyncio.run(s._check_for_updates())
    flush()
    assert s.update_available is True


def test_no_releases_available():
    s = _screen(releases=FakeReleases(latest=None))
    asyncio.run(s._check_for_updates())
    flush()
    assert s.available_version == "—"
    assert s.update_available is False
    assert s.status == "No releases available"


def test_check_failure_is_reported_not_raised():
    s = _screen(releases=FakeReleases(error=ReleaseError("github is down")))
    asyncio.run(s._check_for_updates())
    flush()
    assert "Could not check" in s.status
    assert s.update_available is False


def test_prerelease_preference_is_passed_through():
    r = FakeReleases(latest=FakeRelease())
    s = _screen(releases=r)
    s.allow_prerelease = True
    asyncio.run(s._check_for_updates())
    assert r.asked_prerelease is True


# ── update action ────────────────────────────────────────────────────
class FakeUpdater:
    def __init__(self, error=None):
        self.error = error
        self.applied = None

    async def apply(self, release, workdir=None):
        self.applied = release
        if self.error:
            raise self.error


def _patch_updater(monkeypatch, fake):
    monkeypatch.setattr(
        "dro.components.screens.update_screen.StackUpdater",
        lambda *a, **kw: fake,
    )


def test_successful_update_schedules_a_reboot(monkeypatch, tmp_path):
    fake = FakeUpdater()
    _patch_updater(monkeypatch, fake)
    s = _screen(tmp_path=tmp_path)
    s._latest = FakeRelease()

    asyncio.run(s._run_update())
    flush()
    assert fake.applied is not None
    assert "Restarting" in s.status
    # The reboot is deferred one second so the final status paints first.
    Clock.tick()


def test_failed_update_reports_and_reenables(monkeypatch, tmp_path):
    _patch_updater(monkeypatch, FakeUpdater(error=StackUpdateError("pip exploded")))
    s = _screen(tmp_path=tmp_path)
    s._latest = FakeRelease()
    s.busy = True

    asyncio.run(s._run_update())
    flush()
    assert "Update failed" in s.status
    assert s.busy is False
    assert "_rebooted" not in s.__dict__          # never reboot after a failed install


def test_release_error_during_update_is_handled(monkeypatch, tmp_path):
    _patch_updater(monkeypatch, FakeUpdater(error=ReleaseError("checksum mismatch")))
    s = _screen(tmp_path=tmp_path)
    s._latest = FakeRelease()

    asyncio.run(s._run_update())
    flush()
    assert "Update failed" in s.status
    assert s.busy is False


def test_start_update_is_a_noop_without_a_target():
    s = _screen()
    s._latest = None
    s.start_update()
    assert s.busy is False


def test_start_update_is_a_noop_while_busy():
    s = _screen()
    s._latest = FakeRelease()
    s.busy = True
    started = []
    s._spawn = started.append
    s.start_update()
    assert started == []


# ── status plumbing ──────────────────────────────────────────────────
def test_progress_is_clamped():
    s = _screen()
    s._set_progress(5.0)
    flush()
    assert s.progress == 1.0
    s._set_progress(-3.0)
    flush()
    assert s.progress == 0.0


def test_status_setter_replaces_rather_than_accumulates():
    # The old page appended raw command output forever; this one shows one line.
    s = _screen()
    s._set_status("first")
    flush()
    s._set_status("second")
    flush()
    assert s.status == "second"


def test_reboot_failure_surfaces_a_manual_instruction(monkeypatch):
    s = _screen()

    def boom(*a, **kw):
        raise OSError("permission denied")

    monkeypatch.setattr("dro.components.screens.update_screen.subprocess.Popen", boom)
    s._do_reboot()
    flush()
    assert "restart manually" in s.status
    assert s.busy is False


def test_reboot_invokes_the_configured_command(monkeypatch):
    from dro.components.screens.update_screen import REBOOT_COMMAND

    s = _screen()
    seen = []
    monkeypatch.setattr("dro.components.screens.update_screen.subprocess.Popen", seen.append)
    s._do_reboot()
    assert seen == [REBOOT_COMMAND]


# ── lifecycle / delegation ───────────────────────────────────────────
# NOTE: Screen.__init__ requires a real Window, which the headless mock-GL backend cannot
# provide, so these drive a __new__-built instance like the rest of the suite. __init__
# itself is pure wiring and is marked no-cover in the module.
class FakeManager:
    def __init__(self):
        self.went = None

    def goto(self, name):
        self.went = name


class FakeApp:
    def __init__(self):
        self.manager = FakeManager()


def test_on_pre_enter_resets_and_checks(monkeypatch):
    s = _screen()
    s.app = FakeApp()
    s.status = "stale text"
    s.progress = 0.7
    checked = []
    s.check_for_updates = lambda: checked.append(True)
    monkeypatch.setattr("dro.components.screens.update_screen.installed_version",
                        lambda: "v1.2.0")

    s.on_pre_enter()

    assert s.status == "" and s.progress == 0.0
    assert s.installed_version == "v1.2.0"
    assert checked == [True]


def test_on_pre_enter_does_not_check_while_busy(monkeypatch):
    s = _screen()
    s.busy = True
    checked = []
    s.check_for_updates = lambda: checked.append(True)
    monkeypatch.setattr("dro.components.screens.update_screen.installed_version",
                        lambda: "v1.2.0")
    s.on_pre_enter()
    assert checked == []


def test_check_for_updates_spawns():
    s = _screen()
    spawned = []
    s._spawn = lambda c: (spawned.append(c), c.close())
    s.check_for_updates()
    assert len(spawned) == 1


def test_start_update_spawns_when_ready():
    s = _screen()
    s._latest = FakeRelease()
    spawned = []
    s._spawn = lambda c: (spawned.append(c), c.close())
    s.start_update()
    assert s.busy is True
    assert len(spawned) == 1


def test_goto_advanced_navigates():
    s = _screen()
    s.app = FakeApp()
    s.goto_advanced()
    assert s.app.manager.went == "firmware"


def test_spawn_schedules_on_a_running_loop():
    s = _screen()
    ran = []

    async def work():
        ran.append(True)

    async def drive():
        s._spawn(work())
        await asyncio.sleep(0)          # let the task run

    asyncio.run(drive())
    assert ran == [True]
