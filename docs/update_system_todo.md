# Update System — Monorepo Release & Single-Step Update

> Phased tracker for consolidating software + firmware releases onto a **single version
> tag** published from this monorepo, and collapsing the two-page developer update flow
> into a **one-button user operation**. Mirrors the repo convention (design context inline;
> one-liner checkboxes per phase).
>
> Status: **implemented, unreleased** (2026-08-02). Phases 1–6 done with 100% test
> coverage on the new modules (270 tests green). Phase 7 is cross-repo (`drdro-arch`)
> and Phase 8 needs a real release plus hardware — both still open. Items marked ⚑
> could not be proven from this repo.

---

## Goals

1. **One version for the whole stack.** A single semantic-release tag on `bartei/drdro-mainboard`
   versions firmware *and* software. No independent version streams.
2. **GitHub releases are the single source of truth.** Every artifact the appliance needs is a
   named asset on the release for that tag.
3. **Single-step update.** The user picks a version and presses one button; software and
   firmware are both brought to that version. No separate firmware page visit, no ordering
   for the user to get right.
4. **Coherence by construction.** If the board's firmware version differs from the installed
   software version, the UI raises a warning and requests an immediate update to realign.
5. **A user-facing update page.** Strip developer detail (raw command logs, bank toggles,
   manual reboot, per-artifact version pickers) out of the normal path.
6. **Cut the legacy repos loose.** Nothing points at `drdro-software-f4` or `drdro-firmware-f4`
   any more. This is a new board with a new feature set and no installed base — no migration
   bridge, no backwards compatibility, no version-regression concerns.

### Accepted trade-off

A single version means a **software-only change still reflashes the board** — the firmware
binary differs only in its embedded `FW_VERSION`, but the versions must match. Dual-bank +
rollback makes this safe and it costs ~30 s. Accepted deliberately in exchange for
compatibility guaranteed by construction.

*Escape hatch, if the redundant flashes become annoying:* have the firmware report a separate
`proto` compatibility number alongside `version`, gate the mismatch **warning** on `proto`,
and keep the **update** bringing both to the same tag. Do not build this now — just don't
foreclose it.

### Frozen interface

New software must always be able to drive **older** firmware through
`update` → bootloader → YMODEM. If that path breaks, a mismatched unit can never update
itself out of the mismatch. Treat it as a frozen ABI and document it in `updater.py`.

---

## Phase 1 — Version plumbing (single tag)

- [x] Add `version_toml = ["software/pyproject.toml:project.version"]` to `[tool.semantic_release]` in root `pyproject.toml`.
- [x] Reset `software/pyproject.toml` version from `1.7.0` onto the monorepo stream (no installed base — just renumber).
- [x] Update the root `pyproject.toml` header comment: it currently claims "no source file carries the version", which stops being true.
- [ ] Confirm `semantic-release version` writes and commits the software version bump alongside the changelog commit. ⚑ needs a real release run
- [ ] Confirm `importlib.metadata.version("drdro-software")` reports the release tag after a wheel install. ⚑ wheel builds as `drdro_software-1.2.0`; an actual install was not exercised
- [x] Decide and document the version string format shown in the UI (`v1.3.0` vs `1.3.0`) and normalise both sides to it.

## Phase 2 — Release artifacts (CI)

- [x] Add `tools/build-release.sh` at the repo root as the new `build_command`: call `firmware/tools/build-release.sh`, then build the software artifacts.
- [x] Build the software wheel in CI (`uv build` / `hatchling`) into `software/dist/`.
- [x] Produce `drdro-software.zip` = the `software/` tree **plus** the prebuilt wheel, so installation needs no build backend at runtime.
- [x] Extend `SHA256SUMS.txt` to cover the software zip as well as the firmware images.
- [x] Add `software/dist/*` to `dist_glob_patterns` in root `pyproject.toml`.
- [x] Keep asset names fixed (unversioned) so the client resolves by exact match: `drdro-software.zip`, `drdro-mainboard-app.bin`.
- [x] Update the asset inventory comment block at the top of `.github/workflows/release.yml`.
- [x] Extend the job-summary step to list the software artifacts too.

## Phase 3 — Update client (comms layer)

- [x] Repoint `dro/comms/updater.py`: `GITHUB_REPO = "bartei/drdro-mainboard"`, `APP_ASSET = "drdro-mainboard-app.bin"`.
- [x] **Delete the loose asset fallback** at `updater.py:146-147` (`endswith('.bin') and 'app' in name`) — with one repo and one version it can only ever match the wrong artifact.
- [x] Add a release-resolution helper returning both asset URLs for a given tag (software zip + firmware bin) from one API call.
- [x] Implement download-with-SHA256-verification against the release's `SHA256SUMS.txt`; fail closed on mismatch.
- [x] Document the frozen `update`/bootloader/YMODEM interface in the `updater.py` module docstring.

## Phase 4 — Single-step update flow

Ordering matters: fetch everything before mutating anything, so a dropped link can't strand a
unit with new software and old firmware.

- [x] Step 1 — resolve the target release and both asset URLs.
- [x] Step 2 — download zip + bin and verify both checksums **before** any install action.
- [x] Step 3 — stage the firmware binary to a known path (e.g. `/opt/drdro/staged-firmware.bin`) plus a marker recording the target version.
- [x] Step 4 — install the wheel from the zip into the app venv.
- [x] Step 5 — reboot.
- [x] Step 6 — on startup, if a staged marker matches the installed software version and the board reports a different version, flash the **staged local** binary (no network needed at this point).
- [x] Step 7 — clear the marker on success; leave it in place on failure so the flash retries next boot or from the UI.
- [x] Handle the "board already matches" case — skip the firmware step silently rather than reflashing.
- [x] Handle the reverse case (board *ahead* of software) with a distinct message — real when a board is flashed out of band.
- [ ] Verify a failed flash leaves the unit bootable via dual-bank rollback, and that the retry path recovers it. ⚑ retry path is unit-tested; bootability is a hardware claim, unverified

## Phase 5 — Compatibility check

- [x] Delete `COMPANION_FW_VERSION` from `dro/utils/fw_compat.py` — it is anchored to the dead `drdro-firmware-f4` stream (`v0.6.0`) and can never fire correctly again.
- [x] Rewrite the check as software-version vs board-reported `version`, exact match required.
- [x] Keep `parse_fw_version` for ordering so the banner can distinguish "board behind" from "board ahead".
- [x] Update `Board.firmware_update_required` and the home-screen banner wording for the new rule.
- [x] Make the banner's tap target the consolidated update page.
- [x] Unit tests: equal / behind / ahead / unparseable (dev build) — unparseable must stay quiet, as today.

## Phase 6 — UI simplification

The normal path should be: *what you have → what's available → one button → progress → done.*
Everything else moves behind an advanced section.

**Update page — keep:**
- [x] Installed version (read-only).
- [x] Latest available version, auto-refreshed on entry (drop the manual "Refresh" step).
- [x] One **Update** button covering software + firmware.
- [x] A single progress bar for the whole operation.
- [x] A one-line plain-language status ("Downloading…", "Installing…", "Updating board…", "Restarting…").

**Update page — remove or relocate:**
- [x] Remove the raw command log `TextInput` (`update_screen.kv:66-72`) — `run: git fetch --all`, return codes and stderr are developer output. Route it to the Kivy log instead.
- [x] Remove the "Exit Application" button from the update page — unrelated to updating.
- [x] Move "Allow installation of experimental versions" behind the advanced section.
- [x] Replace the release dropdown with latest-only in the normal path; version picking is an advanced action.

**Firmware page — the "Install from GitHub" section becomes redundant** (firmware version now follows the software version):
- [x] Remove the firmware release list, pre-release toggle and per-version install button from the normal path.
- [x] Move bank selection, "Reboot firmware now", manual firmware install and the activity log into an advanced/developer section.
- [x] Decide whether the advanced controls live on a gated section of the update page or stay as a separate Advanced Firmware screen (recommend: keep the screen, drop it out of the normal navigation).
- [x] Keep the progress bar and current-version/active-bank readouts — those are diagnostics worth having.
- [x] Refresh `dro/help/software_update.md` for the new single-step flow.

## Phase 7 — Appliance image (cross-repo: `drdro-arch`)

- [ ] The git checkout at `/opt/drdro/app` is no longer the update mechanism — decide whether the image still ships a checkout at all. ⚑ cross-repo
- [x] Update `PROJECT_FOLDER` / `VENV_PIP` handling in `update_screen.py` for wheel installation.
- [ ] Ensure the staging path (`/opt/drdro/staged-firmware.bin`) is writable and survives reboot. ⚑ cross-repo
- [ ] Coordinate changes to `drdro-arch/build.sh` and `overlay/opt/drdro/app-run.sh`. ⚑ cross-repo
- [ ] Consider installing into a fresh venv and switching a symlink, so a failed `pip install` can't leave the app unbootable. ⚑ optional hardening, not implemented

## Phase 8 — Validation

- [ ] Dry run on `dev`: confirm a beta release publishes all assets with the expected names and a valid `SHA256SUMS.txt`.
- [ ] Verify the client resolves and downloads both assets from a beta tag.
- [ ] End-to-end on hardware: mismatched unit → single-button update → both versions match, no manual steps.
- [ ] Failure injection: kill the network mid-download; kill power between software install and firmware flash; confirm recovery.
- [ ] Only after the above is green, cut a stable tag from `master`.

---

## Files in scope

| File | Change |
|---|---|
| `pyproject.toml` (root) | `version_toml`, `dist_glob_patterns`, `build_command`, header comment |
| `tools/build-release.sh` | **new** — orchestrates firmware + software artifact builds |
| `firmware/tools/build-release.sh` | extend `SHA256SUMS.txt` to cover software artifacts |
| `.github/workflows/release.yml` | asset inventory comment, job summary |
| `software/pyproject.toml` | version reset onto the monorepo stream |
| `software/dro/comms/updater.py` | repo/asset repoint, drop fallback, checksum verify, frozen-ABI note |
| `software/dro/components/screens/update_screen.py` | single-step flow, wheel install, staging |
| `software/dro/components/screens/update_screen.kv` | simplified user-facing layout |
| `software/dro/components/screens/firmware_screen.py/.kv` | demote to advanced; drop redundant release list |
| `software/dro/utils/fw_compat.py` | drop `COMPANION_FW_VERSION`; compare against own version |
| `software/dro/dispatchers/board.py` | mismatch detection wiring |
| `software/dro/help/software_update.rst` | rewrite for the single-step flow |
| `software/dro/comms/release.py` | **new** — release resolution, download, SHA256 verify |
| `software/dro/comms/stack_update.py` | **new** — fetch/verify/stage/install orchestration |
| `software/dro/comms/firmware_apply.py` | **new** — shared flash-with-settings-preservation |
| `software/dro/comms/pending_update.py` | **new** — post-reboot staged-firmware flash |
| `software/dro/utils/version.py` | **new** — single source of truth for the app's version |
| `software/dro/app.py` | startup hook for the staged flash; version via `installed_version()` |
| `software/dro/components/home/fw_update_banner.py/.kv` | mismatch wording; taps through to Update |
| `software/dro/components/screens/setup_screen.kv` | drop the Firmware entry from normal navigation |
