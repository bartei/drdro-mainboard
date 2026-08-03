"""Apply a local firmware image to the board, preserving settings across the flash.

Shared by both routes that flash a board, so they cannot drift apart:

  * the automatic post-reboot step of the single-step stack update
    (:mod:`dro.comms.stack_update` stages the image; this applies it), and
  * the manual "flash this file" action on the advanced firmware screen.

The image is always local and already checksum-verified by the time it gets here — this
module never touches the network.

Settings survive the flash by two mechanisms, kept from the original firmware screen:
a timestamped profile written before the update (manual rollback), and an in-memory
snapshot replayed afterwards to restore anything a settings-layout change reset to
defaults.
"""
from __future__ import annotations

import asyncio

from kivy.logger import Logger

from dro.comms.updater import FirmwareUpdater, UpdaterError
from dro.profiles import ProfileManager

log = Logger.getChild(__name__)

# How long to wait for the poll loop to pick the board back up after it boots the new bank.
RECONNECT_TRIES = 15
RECONNECT_DELAY = 0.3


async def apply_firmware(board, bin_path: str, *, version: str = "",
                         on_status=None, on_progress=None,
                         updater=None, profiles=None) -> dict:
    """Back up settings, flash ``bin_path``, wait for the board, restore settings.

    Returns the updater's result dict (bank, size, crc, version). Raises
    :class:`~dro.comms.updater.UpdaterError` if the flash itself fails — the caller decides
    whether that is fatal, since dual-bank rollback means the board is still bootable.

    ``updater``/``profiles`` are injectable for testing.
    """
    status = on_status or (lambda *_: None)
    progress = on_progress or (lambda *_: None)
    updater = updater or FirmwareUpdater(board)
    profiles = profiles or ProfileManager(board)

    label = version or "new firmware"

    # ---- 1. back up before touching the firmware ----
    snapshot = None
    try:
        snapshot = await profiles.snapshot_board()
        backup = await profiles.backup_current(
            note=f"automatic backup before firmware update to {label}"
        )
        status(f"Settings backed up to profile '{backup.stem}'")
    except (OSError, UpdaterError) as e:
        # A failed backup must not block the update — the board keeps its own settings in
        # flash, and the dual-bank rollback path is the real safety net.
        log.warning("Settings backup before flash failed: %s", e)
        status("Could not back up settings — continuing")

    # ---- 2. flash ----
    progress(0.0)
    status("Flashing firmware…")
    result = await updater.install(bin_path, on_progress=progress, on_status=status)
    progress(1.0)

    # ---- 3. wait for the board to come back ----
    status("Waiting for the board…")
    for _ in range(RECONNECT_TRIES):
        if board.connected:
            break
        await asyncio.sleep(RECONNECT_DELAY)

    # ---- 4. restore anything the update reset ----
    if board.connected and snapshot:
        try:
            restored = await profiles.restore_board_settings(snapshot)
            status(f"Restored {restored} board setting(s) lost in the update" if restored
                   else "Board settings verified — nothing lost in the update")
        except (OSError, UpdaterError) as e:
            log.warning("Settings restore after flash failed: %s", e)
            status("Could not verify board settings after the update")

    status(f"Firmware updated (bank {result.get('bank')}, version {result.get('version')})")
    return result
