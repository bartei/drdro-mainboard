"""Update screen — one button brings the WHOLE stack to one release.

Software and firmware ship under a single monorepo tag, so there is nothing here for the
user to sequence or choose: the page shows what is installed, what is available, and an
Update button. Everything developer-facing (raw command output, version pickers, bank
selection, manual firmware flashing) lives on the advanced firmware screen instead.

Flow — see dro.comms.stack_update for why the ordering matters:
    download + verify both assets -> stage firmware -> install software -> reboot
    (after reboot) flash the staged firmware, which needs no network.
"""
import asyncio
import os
import subprocess

from kivy.clock import Clock, mainthread
from kivy.logger import Logger
from kivy.properties import BooleanProperty, NumericProperty, StringProperty
from kivy.uix.screenmanager import Screen

from dro.comms.release import ReleaseClient, ReleaseError
from dro.comms.stack_update import Paths, StackUpdateError, StackUpdater
from dro.utils.fw_compat import normalize_version
from dro.utils.kv_loader import load_kv
from dro.utils.version import installed_version

log = Logger.getChild(__name__)
load_kv(__file__)

REBOOT_COMMAND = ["reboot"]


class UpdateScreen(Screen):
    """Minimal user-facing update page. Advanced controls live on the firmware screen."""

    installed_version = StringProperty("")
    available_version = StringProperty("")
    status = StringProperty("")
    progress = NumericProperty(0.0)
    busy = BooleanProperty(False)
    update_available = BooleanProperty(False)
    allow_prerelease = BooleanProperty(False)

    # Pure wiring, and Screen.__init__ needs a real Window — untestable under the headless
    # mock-GL backend the suite runs on. Every method it sets up is covered individually.
    def __init__(self, release_client=None, paths=None, reboot=None, **kv):  # pragma: no cover
        from dro.app import MainApp
        self.app: MainApp = MainApp.get_running_app()
        super().__init__(**kv)
        self.releases = release_client or ReleaseClient()
        self.paths = paths or Paths()
        self._reboot = reboot or self._do_reboot
        self._latest = None
        self.installed_version = installed_version()

    # ── lifecycle ────────────────────────────────────────────────────
    def on_pre_enter(self, *args):
        """Refresh automatically — a manual "check for updates" step is developer ergonomics."""
        self.status = ""
        self.progress = 0.0
        self.installed_version = installed_version()
        if not self.busy:
            self.check_for_updates()

    # ── status helpers (safe from any thread) ────────────────────────
    @mainthread
    def _set_status(self, msg: str):
        log.info("update: %s", msg)
        self.status = msg

    @mainthread
    def _set_progress(self, frac: float):
        self.progress = max(0.0, min(1.0, float(frac)))

    def _spawn(self, coro):
        try:
            asyncio.get_event_loop().create_task(coro)
        except RuntimeError:                       # pragma: no cover — no loop outside the app
            log.error("no running asyncio loop for update task")
            coro.close()

    # ── check ────────────────────────────────────────────────────────
    def check_for_updates(self):
        self._spawn(self._check_for_updates())

    async def _check_for_updates(self):
        self._set_status("Checking for updates…")
        try:
            latest = await self.releases.latest(include_prerelease=self.allow_prerelease)
        except ReleaseError as e:
            self._set_status(f"Could not check for updates: {e}")
            return
        if latest is None:
            self._set_status("No releases available")
            self._set_available(None)
            return
        self._set_available(latest)

    @mainthread
    def _set_available(self, release):
        self._latest = release
        if release is None:
            self.available_version = "—"
            self.update_available = False
            return
        self.available_version = release.version
        same = normalize_version(release.version) == normalize_version(self.installed_version)
        self.update_available = not same
        self.status = "Up to date" if same else f"{release.version} is available"

    # ── update ───────────────────────────────────────────────────────
    def start_update(self):
        if self.busy or self._latest is None:
            return
        self.busy = True
        self._spawn(self._run_update())

    async def _run_update(self):
        updater = StackUpdater(
            self.releases,
            paths=self.paths,
            on_status=self._set_status,
            on_progress=self._set_progress,
        )
        try:
            await updater.apply(self._latest)
        except (ReleaseError, StackUpdateError) as e:
            self._set_status(f"Update failed: {e}")
            self._set_busy(False)
            return
        self._set_status("Restarting…")
        # Give the UI one frame to paint the final status before the box goes down.
        Clock.schedule_once(lambda dt: self._reboot(), 1.0)

    @mainthread
    def _set_busy(self, value: bool):
        self.busy = bool(value)

    def _do_reboot(self):
        """Reboot the appliance so the new software is what comes back up."""
        try:
            subprocess.Popen(REBOOT_COMMAND)
        except OSError as e:
            log.error(f"Reboot failed: {e}")
            self._set_status(f"Please restart manually: {e}")
            self._set_busy(False)

    # ── navigation ───────────────────────────────────────────────────
    def goto_advanced(self):
        self.app.manager.goto("firmware")
