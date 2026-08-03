"""Post-reboot half of the single-step update: flash the staged firmware image.

The stack update installs new software, stages the matching firmware image and reboots.
On the way back up this controller notices the staged image, waits for the board, and
flashes it — from local disk, so the network is not needed at this point. That is the whole
reason the image is downloaded *before* the reboot.

Retry semantics: the marker is cleared only on success, or when it is stale (naming a
version other than the installed one). A failed flash therefore retries on the next boot,
and dual-bank rollback means the board stays bootable in the meantime.
"""
from __future__ import annotations

import asyncio

from kivy.logger import Logger

from dro.comms.firmware_apply import apply_firmware
from dro.comms.stack_update import Paths, clear_pending, pending_firmware_update
from dro.comms.updater import UpdaterError
from dro.utils.fw_compat import versions_match
from dro.utils.version import installed_version

log = Logger.getChild(__name__)

CONNECT_TIMEOUT = 30.0        # give the board this long to appear before giving up this boot
CONNECT_POLL = 0.5


class PendingUpdateController:
    """Applies a staged firmware image on startup, if there is one to apply."""

    def __init__(self, board, paths: Paths | None = None,
                 software_version: str | None = None,
                 on_status=None, on_progress=None, apply_fn=None):
        self.board = board
        self.paths = paths or Paths()
        self.software_version = software_version or installed_version()
        self._status = on_status or (lambda *_: None)
        self._progress = on_progress or (lambda *_: None)
        self._apply = apply_fn or apply_firmware

    async def run(self) -> bool:
        """Apply the staged update if one is pending. True when a flash was performed."""
        pending = pending_firmware_update(self.paths, self.software_version)
        if pending is None:
            return False

        log.info("Staged firmware update pending for %s", pending.version)
        self._status(f"Finishing update to {pending.version}…")

        if not await self._wait_for_board():
            # Board never showed up — keep the marker so the next boot tries again.
            self._status("Board not reachable — firmware update will retry")
            return False

        # The board may already be on the target version (a retry after the flash
        # succeeded but the marker survived, or an out-of-band flash). Nothing to do.
        if versions_match(self.board.firmware_version, pending.version):
            log.info("Board already on %s — clearing staged update", pending.version)
            clear_pending(self.paths)
            self._status("")
            return False

        try:
            await self._apply(
                self.board, str(pending.firmware_path),
                version=pending.version,
                on_status=self._status, on_progress=self._progress,
            )
        except (UpdaterError, OSError) as e:
            log.error(f"Staged firmware update failed: {e}")
            self._status(f"Firmware update failed — will retry on next start: {e}")
            return False

        clear_pending(self.paths)
        # Re-evaluate coherence so the banner clears without waiting for a reconnect.
        refresh = getattr(self.board, "_refresh_version_coherence", None)
        if refresh is not None:
            refresh()
        self._status(f"Update to {pending.version} complete")
        return True

    async def _wait_for_board(self) -> bool:
        waited = 0.0
        while waited < CONNECT_TIMEOUT:
            if self.board.connected:
                return True
            await asyncio.sleep(CONNECT_POLL)
            waited += CONNECT_POLL
        return False
