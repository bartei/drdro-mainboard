"""Tests for the single-step stack update (pre-reboot half).

The behaviours that make the flow survivable, pinned here:
  * nothing is staged or installed until BOTH assets are downloaded and verified
  * the staging marker is written atomically, and last
  * a stale marker (naming a version other than the installed one) is discarded, never applied
  * any failure after staging clears the marker, so a reboot can't flash an orphan image
  * zip extraction refuses absolute paths and traversal
"""
import asyncio
import hashlib
import json
import zipfile
from pathlib import Path

import pytest

from dro.comms.release import FIRMWARE_ASSET, SOFTWARE_ASSET, ReleaseError
from dro.comms.stack_update import (
    Paths,
    StackUpdateError,
    StackUpdater,
    clear_pending,
    find_wheel,
    install_wheel,
    pending_firmware_update,
    stage_firmware,
)


# ── fixtures / doubles ───────────────────────────────────────────────
class FakeRelease:
    def __init__(self, version="v1.2.0"):
        self.version = version
        self.tag = version
        self.software_url = "https://dl/sw.zip"
        self.firmware_url = "https://dl/fw.bin"
        self.checksum_url = "https://dl/SHA256SUMS.txt"


class FakeReleaseClient:
    """Writes deterministic payloads and reports checksums that match them."""

    def __init__(self, sw_body=b"softwarezip", fw_body=b"firmwarebin", sums=None,
                 fail_on=None):
        self.sw_body = sw_body
        self.fw_body = fw_body
        self._sums = sums
        self.fail_on = fail_on or set()
        self.downloaded = []

    async def fetch_checksums(self, release):
        if "checksums" in self.fail_on:
            raise ReleaseError("manifest unavailable")
        if self._sums is not None:
            return self._sums
        return {
            SOFTWARE_ASSET: hashlib.sha256(self.sw_body).hexdigest(),
            FIRMWARE_ASSET: hashlib.sha256(self.fw_body).hexdigest(),
        }

    async def download_verified(self, url, dest, name, sums, on_progress=None):
        if name in self.fail_on:
            raise ReleaseError(f"{name}: checksum mismatch")
        body = self.sw_body if name == SOFTWARE_ASSET else self.fw_body
        Path(dest).parent.mkdir(parents=True, exist_ok=True)
        Path(dest).write_bytes(body)
        self.downloaded.append(name)
        if on_progress:
            on_progress(1.0)
        return str(dest)


def make_zip(path, wheel_name="drdro_software-1.2.0-py3-none-any.whl",
             wheel_at="software/dist", extra=None):
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("software/pyproject.toml", "[project]\nname='drdro-software'\n")
        zf.writestr("software/dro/__init__.py", "")
        if wheel_name:
            zf.writestr(f"{wheel_at}/{wheel_name}", "wheel-bytes")
        for name, data in (extra or {}).items():
            zf.writestr(name, data)
    return path


@pytest.fixture
def paths(tmp_path):
    return Paths(root=tmp_path / "opt-drdro")


# ── find_wheel ───────────────────────────────────────────────────────
def test_find_wheel_locates_bundled_wheel(tmp_path):
    z = make_zip(tmp_path / "sw.zip")
    wheel = find_wheel(z, tmp_path / "out")
    assert wheel.name.endswith(".whl")
    assert wheel.parent.name == "dist"


def test_find_wheel_falls_back_to_any_wheel(tmp_path):
    z = make_zip(tmp_path / "sw.zip", wheel_at="somewhere/else")
    assert find_wheel(z, tmp_path / "out").name.endswith(".whl")


def test_find_wheel_errors_when_no_wheel(tmp_path):
    z = make_zip(tmp_path / "sw.zip", wheel_name=None)
    with pytest.raises(StackUpdateError, match="no wheel"):
        find_wheel(z, tmp_path / "out")


def test_find_wheel_picks_first_of_several(tmp_path):
    z = make_zip(tmp_path / "sw.zip", extra={"software/dist/aaa-1-py3-none-any.whl": "x"})
    assert find_wheel(z, tmp_path / "out").name.startswith("aaa")


def test_find_wheel_rejects_bad_zip(tmp_path):
    p = tmp_path / "bad.zip"
    p.write_bytes(b"not a zip at all")
    with pytest.raises(StackUpdateError, match="not a valid zip"):
        find_wheel(p, tmp_path / "out")


def test_find_wheel_rejects_path_traversal(tmp_path):
    p = tmp_path / "evil.zip"
    with zipfile.ZipFile(p, "w") as zf:
        zf.writestr("../escaped.txt", "pwned")
    with pytest.raises(StackUpdateError, match="unsafe path"):
        find_wheel(p, tmp_path / "out")
    assert not (tmp_path / "escaped.txt").exists()


@pytest.mark.parametrize("evil", [
    "/abs/rooted.txt",          # POSIX absolute — NOT absolute per pathlib on Windows
    "C:/windows/rooted.txt",    # drive-qualified
    "sub/../../escaped.txt",    # traversal below the root
    "..\\backslash-escape.txt",  # backslash separator
])
def test_find_wheel_rejects_unsafe_member(tmp_path, evil):
    p = tmp_path / "evil.zip"
    with zipfile.ZipFile(p, "w") as zf:
        # writestr() normalises a leading "/" away, so set the stored name explicitly —
        # otherwise the test silently stops testing what it claims to.
        zf.writestr(zipfile.ZipInfo(filename=evil), "pwned")
    with pytest.raises(StackUpdateError, match="unsafe path"):
        find_wheel(p, tmp_path / "out")
    assert not (tmp_path / "escaped.txt").exists()


# ── install_wheel ────────────────────────────────────────────────────
def test_install_wheel_uses_venv_pip_when_present(tmp_path, paths):
    paths.venv_pip.parent.mkdir(parents=True, exist_ok=True)
    paths.venv_pip.write_text("#!/bin/sh\n")
    calls = []

    install_wheel(Path("w.whl"), paths, runner=lambda c: (calls.append(c), (0, ""))[1])

    assert calls[0][0] == str(paths.venv_pip)
    assert "--upgrade" in calls[0] and "--no-input" in calls[0]


def test_install_wheel_falls_back_to_bare_pip(tmp_path, paths):
    calls = []
    install_wheel(Path("w.whl"), paths, runner=lambda c: (calls.append(c), (0, ""))[1])
    assert calls[0][0] == "pip"


def test_install_wheel_raises_on_failure(paths):
    with pytest.raises(StackUpdateError, match="pip install failed"):
        install_wheel(Path("w.whl"), paths, runner=lambda c: (1, "boom"))


# ── staging ──────────────────────────────────────────────────────────
def test_stage_firmware_writes_image_and_marker(tmp_path, paths):
    src = tmp_path / "fw.bin"
    src.write_bytes(b"FIRMWARE")

    dest = stage_firmware(src, "v1.2.0", paths)

    assert dest.read_bytes() == b"FIRMWARE"
    marker = json.loads(paths.marker.read_text())
    assert marker == {"version": "v1.2.0", "firmware": dest.name}


def test_stage_firmware_creates_root(tmp_path):
    p = Paths(root=tmp_path / "deep" / "nested")
    src = tmp_path / "fw.bin"
    src.write_bytes(b"X")
    stage_firmware(src, "v1.0.0", p)
    assert p.staged_firmware.exists()


def test_stage_firmware_leaves_no_tmp_marker(tmp_path, paths):
    src = tmp_path / "fw.bin"
    src.write_bytes(b"X")
    stage_firmware(src, "v1.0.0", paths)
    assert not paths.marker.with_suffix(".tmp").exists()


# ── pending / clear ──────────────────────────────────────────────────
def test_pending_none_when_no_marker(paths):
    assert pending_firmware_update(paths, "v1.2.0") is None


def test_pending_returns_staged_update(tmp_path, paths):
    src = tmp_path / "fw.bin"
    src.write_bytes(b"X")
    stage_firmware(src, "v1.2.0", paths)

    pending = pending_firmware_update(paths, "v1.2.0")
    assert pending.version == "v1.2.0"
    assert pending.firmware_path == paths.staged_firmware


def test_pending_matches_across_version_spellings(tmp_path, paths):
    src = tmp_path / "fw.bin"
    src.write_bytes(b"X")
    stage_firmware(src, "v1.2.0", paths)
    # A dev build of the same tag is the same version.
    assert pending_firmware_update(paths, "1.2.0-4-gabc1234") is not None


def test_pending_discards_stale_marker(tmp_path, paths):
    # Marker for a version the software is NOT on — flashing it would push the board to a
    # version the software never reached.
    src = tmp_path / "fw.bin"
    src.write_bytes(b"X")
    stage_firmware(src, "v1.3.0", paths)

    assert pending_firmware_update(paths, "v1.2.0") is None
    assert not paths.marker.exists()
    assert not paths.staged_firmware.exists()


def test_pending_discards_unreadable_marker(paths):
    paths.root.mkdir(parents=True, exist_ok=True)
    paths.marker.write_text("{not json")

    assert pending_firmware_update(paths, "v1.2.0") is None
    assert not paths.marker.exists()


def test_pending_discards_marker_without_image(paths):
    paths.root.mkdir(parents=True, exist_ok=True)
    paths.marker.write_text(json.dumps({"version": "v1.2.0", "firmware": "gone.bin"}))

    assert pending_firmware_update(paths, "v1.2.0") is None
    assert not paths.marker.exists()


def test_clear_pending_is_idempotent(paths):
    clear_pending(paths)                     # nothing exists yet — must not raise
    paths.root.mkdir(parents=True, exist_ok=True)
    paths.marker.write_text("{}")
    paths.staged_firmware.write_bytes(b"X")
    clear_pending(paths)
    assert not paths.marker.exists() and not paths.staged_firmware.exists()


# ── StackUpdater ─────────────────────────────────────────────────────
def test_prepare_downloads_both_then_stages(tmp_path, paths):
    client = FakeReleaseClient()
    statuses, progress = [], []
    up = StackUpdater(client, paths=paths, on_status=statuses.append,
                      on_progress=progress.append)

    zip_path = asyncio.run(up.prepare(FakeRelease(), workdir=tmp_path / "work"))

    assert client.downloaded == [SOFTWARE_ASSET, FIRMWARE_ASSET]
    assert Path(zip_path).read_bytes() == b"softwarezip"
    assert paths.staged_firmware.read_bytes() == b"firmwarebin"
    assert progress == sorted(progress) and progress[-1] <= 1.0
    assert any("Downloading software" in s for s in statuses)
    assert any("Downloading firmware" in s for s in statuses)


def test_prepare_does_not_stage_when_firmware_fails(tmp_path, paths):
    client = FakeReleaseClient(fail_on={FIRMWARE_ASSET})
    up = StackUpdater(client, paths=paths)

    with pytest.raises(ReleaseError):
        asyncio.run(up.prepare(FakeRelease(), workdir=tmp_path / "work"))

    # Fetch-everything-before-mutating: a firmware failure must leave nothing staged.
    assert not paths.staged_firmware.exists()
    assert not paths.marker.exists()


def test_prepare_does_not_download_firmware_when_software_fails(tmp_path, paths):
    client = FakeReleaseClient(fail_on={SOFTWARE_ASSET})
    up = StackUpdater(client, paths=paths)

    with pytest.raises(ReleaseError):
        asyncio.run(up.prepare(FakeRelease(), workdir=tmp_path / "work"))
    assert client.downloaded == []


def test_prepare_propagates_manifest_failure(tmp_path, paths):
    client = FakeReleaseClient(fail_on={"checksums"})
    up = StackUpdater(client, paths=paths)
    with pytest.raises(ReleaseError, match="manifest unavailable"):
        asyncio.run(up.prepare(FakeRelease(), workdir=tmp_path / "work"))


def test_apply_installs_and_leaves_staged_firmware(tmp_path, paths):
    zip_bytes = make_zip(tmp_path / "src.zip").read_bytes()
    client = FakeReleaseClient(sw_body=zip_bytes)
    calls = []
    up = StackUpdater(client, paths=paths, runner=lambda c: (calls.append(c), (0, ""))[1])

    asyncio.run(up.apply(FakeRelease(), workdir=tmp_path / "work"))

    assert any("install" in c for c in calls[0])
    # The staged image survives apply() — it is flashed after the reboot, not now.
    assert paths.staged_firmware.exists()
    assert paths.marker.exists()


def test_apply_clears_staging_when_install_fails(tmp_path, paths):
    zip_bytes = make_zip(tmp_path / "src.zip").read_bytes()
    client = FakeReleaseClient(sw_body=zip_bytes)
    up = StackUpdater(client, paths=paths, runner=lambda c: (1, "pip exploded"))

    with pytest.raises(StackUpdateError):
        asyncio.run(up.apply(FakeRelease(), workdir=tmp_path / "work"))

    # Software never installed, so the staged firmware belongs to nothing — it must go,
    # or the next boot would flash the board to a version the software isn't on.
    assert not paths.staged_firmware.exists()
    assert not paths.marker.exists()


def test_apply_clears_staging_when_zip_is_broken(tmp_path, paths):
    client = FakeReleaseClient(sw_body=b"definitely not a zip")
    up = StackUpdater(client, paths=paths, runner=lambda c: (0, ""))

    with pytest.raises(StackUpdateError):
        asyncio.run(up.apply(FakeRelease(), workdir=tmp_path / "work"))
    assert not paths.marker.exists()


def test_apply_clears_staging_on_download_failure(tmp_path, paths):
    client = FakeReleaseClient(fail_on={FIRMWARE_ASSET})
    up = StackUpdater(client, paths=paths)
    with pytest.raises(ReleaseError):
        asyncio.run(up.apply(FakeRelease(), workdir=tmp_path / "work"))
    assert not paths.marker.exists()


def test_apply_reports_progress_to_completion(tmp_path, paths):
    zip_bytes = make_zip(tmp_path / "src.zip").read_bytes()
    client = FakeReleaseClient(sw_body=zip_bytes)
    progress = []
    up = StackUpdater(client, paths=paths, runner=lambda c: (0, ""),
                      on_progress=progress.append)

    asyncio.run(up.apply(FakeRelease(), workdir=tmp_path / "work"))
    assert progress[-1] == 1.0


def test_real_runner_executes_a_command():
    # The default runner is injected away everywhere else; exercise it once so the real
    # subprocess plumbing is not shipped untested.
    import sys

    from dro.comms.stack_update import _run

    rc, out = _run([sys.executable, "-c", "print('hello-from-runner')"])
    assert rc == 0
    assert "hello-from-runner" in out


def test_real_runner_reports_failure_and_output():
    import sys

    from dro.comms.stack_update import _run

    rc, out = _run([sys.executable, "-c", "import sys; sys.stderr.write('nope'); sys.exit(3)"])
    assert rc == 3
    assert "nope" in out                      # stderr is folded into stdout


def test_clear_pending_survives_unremovable_entries(paths):
    # A marker path that is a directory cannot be unlinked; clearing must warn, not raise,
    # or a stuck file would make every subsequent update attempt explode.
    paths.root.mkdir(parents=True, exist_ok=True)
    paths.marker.mkdir()
    clear_pending(paths)
    assert paths.marker.exists()              # still there, but no exception escaped


def test_apply_wraps_unexpected_os_errors(tmp_path):
    # Staging into a root whose parent is a FILE raises NotADirectoryError from mkdir —
    # an OSError that is neither ReleaseError nor StackUpdateError.
    blocker = tmp_path / "blocker"
    blocker.write_text("i am a file")
    bad_paths = Paths(root=blocker / "opt-drdro")
    up = StackUpdater(FakeReleaseClient(), paths=bad_paths, runner=lambda c: (0, ""))

    with pytest.raises(StackUpdateError, match="update failed"):
        asyncio.run(up.apply(FakeRelease(), workdir=tmp_path / "work"))


def test_default_paths_point_at_the_appliance_layout():
    p = Paths()
    assert str(p.staged_firmware).endswith("staged-firmware.bin")
    assert str(p.marker).endswith("staged-update.json")
    assert "opt" in str(p.root)
