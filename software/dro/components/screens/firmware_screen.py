"""Advanced firmware screen — bank control, manual flashing, diagnostics.

NOT the normal update path. Under the single-version design the user updates the whole
stack from the Update screen with one button; firmware follows the software automatically.
What remains here is what a developer or a recovery situation needs:

  * which version / bank the board is actually running
  * forcing the boot bank and rebooting the firmware
  * flashing a local .bin by hand (including the image staged by a stack update)
  * the raw activity log

There is deliberately no "pick a firmware version from GitHub" list any more: firmware
version is not independently selectable — it is whatever the installed software's release
carries. Offering a picker here would let a user create exactly the mismatch the design
exists to prevent.
"""
import asyncio
import os

from kivy.clock import mainthread
from kivy.logger import Logger
from kivy.properties import BooleanProperty, NumericProperty, StringProperty
from kivy.uix.screenmanager import Screen

from dro.comms.firmware_apply import apply_firmware
from dro.comms.stack_update import Paths, clear_pending, pending_firmware_update
from dro.comms.updater import FirmwareUpdater, UpdaterError
from dro.utils.kv_loader import load_kv
from dro.utils.version import installed_version

log = Logger.getChild(__name__)
load_kv(__file__)


class FirmwareScreen(Screen):
    current_version = StringProperty("—")
    software_version = StringProperty("")
    active_bank_text = StringProperty("—")
    boot_bank = StringProperty("")
    progress = NumericProperty(0.0)
    busy = BooleanProperty(False)
    status_text = StringProperty("")
    staged_version = StringProperty("")        # a staged image waiting to be flashed, if any

    # Pure wiring, and Screen.__init__ needs a real Window — untestable under the headless
    # mock-GL backend the suite runs on. Every method it sets up is covered individually.
    def __init__(self, paths=None, **kv):  # pragma: no cover
        from dro.app import MainApp
        self.app: MainApp = MainApp.get_running_app()
        super().__init__(**kv)
        self.paths = paths or Paths()
        self._updater: FirmwareUpdater | None = None
        self.software_version = installed_version()

    @property
    def updater(self) -> FirmwareUpdater:
        if self._updater is None:
            self._updater = FirmwareUpdater(self.app.board)
        return self._updater

    # ── status helpers (safe from any thread) ───────────────────────
    @mainthread
    def _status(self, msg: str):
        log.info("firmware: %s", msg)
        self.status_text = (self.status_text + msg + "\n")[-4000:]

    @mainthread
    def _set_progress(self, frac: float):
        self.progress = max(0.0, min(1.0, float(frac)))

    @mainthread
    def _set_busy(self, value: bool):
        self.busy = bool(value)

    def _spawn(self, coro):
        try:
            asyncio.get_event_loop().create_task(coro)
        except RuntimeError:                     # pragma: no cover — no loop outside the app
            log.error("no running asyncio loop for firmware task")
            coro.close()

    # ── lifecycle ────────────────────────────────────────────────────
    def on_pre_enter(self, *args):
        self.software_version = installed_version()
        self._refresh_staged()
        self.refresh_status()

    def _refresh_staged(self):
        pending = pending_firmware_update(self.paths, self.software_version)
        self.staged_version = pending.version if pending else ""

    def refresh_status(self):
        self._spawn(self._refresh_status())

    async def _refresh_status(self):
        if not self.app.board.connected:
            self.current_version = "(offline)"
            self.active_bank_text = "—"
            return
        ver = await self.updater.get_version()
        bank = await self.updater.get_active_bank()
        self.current_version = ver or "—"
        self.active_bank_text = "—" if bank is None else str(bank)
        if bank is not None:
            self.boot_bank = str(bank)

    # ── bank selector / reset ────────────────────────────────────────
    def set_bank(self, value: str):
        if value not in ("0", "1"):
            return
        self.boot_bank = value                       # reflect the selection immediately
        if value != self.active_bank_text:
            self._spawn(self._set_bank(int(value)))

    async def _set_bank(self, bank: int):
        self._status(f"Setting active bank → {bank}")
        ok = await self.updater.set_active_bank(bank)
        self._status("Bank set (effective next boot)" if ok else "Bank set failed")
        await self._refresh_status()

    def do_reset(self):
        self._status("Rebooting firmware…")
        self._spawn(self._reset())

    async def _reset(self):
        await self.updater.reset()
        await asyncio.sleep(2.5)
        await self._refresh_status()
        self._status("Firmware rebooted")

    # ── manual flash ─────────────────────────────────────────────────
    def flash_staged(self):
        """Flash the image a stack update staged but did not finish applying."""
        if self.busy:
            return
        pending = pending_firmware_update(self.paths, self.software_version)
        if pending is None:
            self._status("No staged firmware image to flash")
            return
        self._spawn(self._flash(str(pending.firmware_path), pending.version,
                                clear_staged=True))

    def flash_path(self, path: str):
        """Flash an arbitrary local .bin — the recovery path."""
        if self.busy:
            return
        if not path or not os.path.exists(path):
            self._status(f"No such file: {path}")
            return
        self._spawn(self._flash(path, ""))

    async def _flash(self, bin_path: str, version: str, clear_staged: bool = False):
        self._set_busy(True)
        self._set_progress(0.0)
        try:
            await apply_firmware(
                self.app.board, bin_path, version=version,
                on_status=self._status, on_progress=self._set_progress,
            )
            # A staged image flashed by hand is spent — drop the marker, or the next boot
            # would pick it up again. (The startup controller would notice the versions
            # already agree and discard it, but leaving it is needless ambiguity.)
            if clear_staged:
                clear_pending(self.paths)
            await self._refresh_status()
            self._refresh_staged()
        except (UpdaterError, OSError) as e:
            log.exception("firmware flash failed")
            self._status(f"Flash FAILED: {e}")
        finally:
            self._set_busy(False)
