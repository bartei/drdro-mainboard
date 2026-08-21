"""Single-step stack update: software + firmware brought to one release, one button.

Ordering is the whole design. Everything is fetched and verified **before** anything is
mutated, so a dropped link can never strand a unit with new software and old firmware:

    1. resolve the target release (both asset URLs from one API call)
    2. download + SHA256-verify the software zip AND the firmware image
    3. stage the firmware image to a fixed path, with a marker naming the target version
    4. install the software wheel out of the zip
    5. reboot
    -- reboot --
    6. on startup, if the marker matches the installed software and the board disagrees,
       flash the STAGED LOCAL image — no network needed at this point
    7. clear the marker on success; leave it on failure so the flash retries

Step 6 lives in :func:`pending_firmware_update`, which the app calls at startup; the flash
itself is :class:`dro.comms.updater.FirmwareUpdater`'s job.

FROZEN INTERFACE: new software must always be able to drive *older* firmware through
``update`` → bootloader → YMODEM. If that path ever breaks compatibility, a mismatched
unit can no longer update itself out of the mismatch, and the single-version design loses
its only recovery route. See ``dro.comms.updater``.

Kivy-free and injectable (``runner``, ``paths``) so the whole flow is testable without a
board, a network or a filesystem of consequence.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from kivy.logger import Logger

from dro.comms.release import (
    FIRMWARE_ASSET,
    SOFTWARE_ASSET,
    ReleaseError,
)

log = Logger.getChild(__name__)

# Appliance layout (drdro-arch image; see overlay/opt/drdro/app-run.sh). The staging paths
# must live on persistent storage — step 6 reads them after a reboot.
DEFAULT_ROOT = Path("/opt/drdro")
STAGED_FIRMWARE_NAME = "staged-firmware.bin"
STAGED_MARKER_NAME = "staged-update.json"


class StackUpdateError(Exception):
    """A step of the single-step update failed."""


@dataclass(frozen=True)
class Paths:
    """Filesystem layout for staging and installation. Injectable for tests."""

    root: Path = DEFAULT_ROOT

    @property
    def staged_firmware(self) -> Path:
        return self.root / STAGED_FIRMWARE_NAME

    @property
    def marker(self) -> Path:
        return self.root / STAGED_MARKER_NAME

    @property
    def venv_pip(self) -> Path:
        return self.root / "app" / ".venv" / "bin" / "pip"


@dataclass(frozen=True)
class PendingUpdate:
    """A staged firmware image awaiting a flash, recovered from the marker after reboot."""

    version: str
    firmware_path: Path


def _run(cmd: list[str]) -> tuple[int, str]:
    """Run a command, returning (returncode, combined output). Replaceable in tests."""
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout or ""


def _reject_unsafe_member(member: str) -> None:
    """Raise if a zip entry name would escape the extraction directory.

    Checked against POSIX semantics and raw strings rather than ``pathlib.Path``: zip
    entry names are always ``/``-separated, and ``Path("/abs/x").is_absolute()`` is False
    on Windows — so a host-dependent check would let a POSIX-absolute entry through on
    exactly the platform where the developer is least likely to notice.
    """
    name = member.replace("\\", "/")
    if name.startswith("/"):
        raise StackUpdateError(f"unsafe path in {SOFTWARE_ASSET}: {member}")
    # Drive-qualified ("C:/x") or UNC-ish names are absolute on Windows.
    if len(name) >= 2 and name[1] == ":":
        raise StackUpdateError(f"unsafe path in {SOFTWARE_ASSET}: {member}")
    if ".." in PurePosixPath(name).parts:
        raise StackUpdateError(f"unsafe path in {SOFTWARE_ASSET}: {member}")


def find_wheel(zip_path, extract_to) -> Path:
    """Extract ``drdro-software.zip`` and return the bundled wheel.

    The zip carries ``software/dist/*.whl`` built by CI precisely so installation needs no
    build backend at runtime — a source-tree install would pull hatchling from PyPI, making
    an appliance update depend on more than GitHub being reachable.
    """
    extract_to = Path(extract_to)
    try:
        with zipfile.ZipFile(zip_path) as zf:
            # Reject absolute paths and traversal before writing anything to disk.
            for member in zf.namelist():
                _reject_unsafe_member(member)
            zf.extractall(extract_to)
    except zipfile.BadZipFile as e:
        raise StackUpdateError(f"{SOFTWARE_ASSET} is not a valid zip: {e}") from e

    wheels = sorted(extract_to.glob("software/dist/*.whl")) or sorted(
        extract_to.glob("**/*.whl")
    )
    if not wheels:
        raise StackUpdateError(
            f"{SOFTWARE_ASSET} contains no wheel — the release was built incorrectly"
        )
    if len(wheels) > 1:
        log.warning("Multiple wheels in %s, using %s", SOFTWARE_ASSET, wheels[0].name)
    return wheels[0]


def install_wheel(wheel: Path, paths: Paths, runner=_run) -> None:
    """``pip install --upgrade`` the wheel into the app venv.

    ``--no-deps`` is deliberately NOT used: a release may add a dependency, and that has to
    resolve. ``--no-input`` keeps pip from ever blocking on a prompt in a kiosk session.
    """
    pip = str(paths.venv_pip) if paths.venv_pip.exists() else "pip"
    rc, out = runner([pip, "install", "--no-input", "--upgrade", str(wheel)])
    if rc != 0:
        raise StackUpdateError(f"pip install failed (rc={rc}): {out.strip()[-800:]}")
    log.info("Installed %s", wheel.name)


def stage_firmware(src, version: str, paths: Paths) -> Path:
    """Copy the verified firmware image to its post-reboot home and write the marker.

    Written marker-last: the marker is the commitment, so it must not exist until the image
    beside it is complete. A crash between the two leaves a stray .bin and no marker, which
    the next run simply overwrites.
    """
    paths.root.mkdir(parents=True, exist_ok=True)
    dest = paths.staged_firmware
    shutil.copyfile(src, dest)
    payload = {"version": version, "firmware": dest.name}
    tmp = paths.marker.with_suffix(".tmp")
    tmp.write_text(json.dumps(payload), encoding="utf-8")
    os.replace(tmp, paths.marker)          # atomic — no half-written marker
    log.info("Staged firmware %s for %s", dest, version)
    return dest


def pending_firmware_update(paths: Paths, software_version: str) -> PendingUpdate | None:
    """The staged update to apply on this boot, or ``None``.

    Returns ``None`` — and clears the marker — when the marker is for a version other than
    the one now installed. That is a stale marker from an update that was superseded or
    rolled back, and flashing its image would push the board to a version the software no
    longer is.
    """
    from dro.utils.fw_compat import normalize_version

    if not paths.marker.exists():
        return None
    try:
        data = json.loads(paths.marker.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        log.warning("Unreadable staged-update marker (%s) — discarding", e)
        clear_pending(paths)
        return None

    version = str(data.get("version") or "")
    firmware = paths.root / str(data.get("firmware") or STAGED_FIRMWARE_NAME)
    if normalize_version(version) != normalize_version(software_version):
        log.info("Discarding stale staged update for %s (software is %s)",
                 version, software_version)
        clear_pending(paths)
        return None
    if not firmware.exists():
        log.warning("Staged marker for %s but %s is missing — discarding", version, firmware)
        clear_pending(paths)
        return None
    return PendingUpdate(version=version, firmware_path=firmware)


def clear_pending(paths: Paths) -> None:
    """Remove the marker and the staged image. Safe to call when neither exists."""
    for p in (paths.marker, paths.staged_firmware):
        try:
            p.unlink()
        except FileNotFoundError:
            pass
        except OSError as e:
            log.warning("Could not remove %s: %s", p, e)


class StackUpdater:
    """Drives the fetch → verify → stage → install half of the single-step update.

    The flash half happens after the reboot, from the staged image. ``on_status`` takes a
    short user-facing string; ``on_progress`` a 0..1 fraction for the whole operation.
    """

    # Fractions of the overall progress bar. Downloads dominate wall-clock, so they own
    # most of the bar and the install steps get the tail.
    _SW_SHARE = 0.55
    _FW_SHARE = 0.35

    def __init__(self, release_client, paths: Paths | None = None,
                 runner=_run, on_status=None, on_progress=None):
        self.releases = release_client
        self.paths = paths or Paths()
        self._runner = runner
        self._status = on_status or (lambda *_: None)
        self._progress = on_progress or (lambda *_: None)

    async def prepare(self, release, workdir=None) -> Path:
        """Download + verify both assets and stage the firmware. Returns the software zip.

        Nothing outside ``workdir`` and the staging path is touched, and the staging write
        happens only after both downloads have passed verification.
        """
        tmp = Path(workdir or tempfile.mkdtemp(prefix="drdro-update-"))
        tmp.mkdir(parents=True, exist_ok=True)

        self._status("Checking release…")
        sums = await self.releases.fetch_checksums(release)

        zip_path = tmp / SOFTWARE_ASSET
        bin_path = tmp / FIRMWARE_ASSET

        self._status(f"Downloading software {release.version}…")
        await self.releases.download_verified(
            release.software_url, zip_path, SOFTWARE_ASSET, sums,
            on_progress=lambda f: self._progress(f * self._SW_SHARE),
        )

        self._status(f"Downloading firmware {release.version}…")
        await self.releases.download_verified(
            release.firmware_url, bin_path, FIRMWARE_ASSET, sums,
            on_progress=lambda f: self._progress(
                self._SW_SHARE + f * self._FW_SHARE),
        )

        self._status("Staging firmware…")
        stage_firmware(bin_path, release.version, self.paths)
        self._progress(self._SW_SHARE + self._FW_SHARE)
        return zip_path

    async def apply(self, release, workdir=None) -> None:
        """Full pre-reboot half: prepare, then install the software.

        On failure after staging, the marker is cleared — the staged image belongs to a
        version that is not installed, and leaving it would flash the board to a version
        the software never reached.
        """
        tmp = Path(workdir or tempfile.mkdtemp(prefix="drdro-update-"))
        try:
            zip_path = await self.prepare(release, workdir=tmp)
            self._status("Installing software…")
            wheel = find_wheel(zip_path, tmp / "unpacked")
            install_wheel(wheel, self.paths, runner=self._runner)
            self._progress(1.0)
            self._status(f"Installed {release.version} — restarting")
        except (ReleaseError, StackUpdateError):
            clear_pending(self.paths)
            raise
        except OSError as e:
            clear_pending(self.paths)
            raise StackUpdateError(f"update failed: {e}") from e
