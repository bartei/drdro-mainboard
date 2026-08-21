#!/usr/bin/env bash
#
# Monorepo release build — ONE version, all artifacts.
#
# python-semantic-release invokes this as `build_command` (see the root pyproject.toml)
# with NEW_VERSION exported, after it has stamped software/pyproject.toml. It produces
# everything `semantic-release publish` uploads (dist_glob_patterns):
#
#   firmware/dist/drdro-mainboard-app.{bin,hex,elf}          application (the .bin is what
#                                                            the host updater flashes)
#   firmware/dist/drdro-mainboard-bootloader.{bin,hex,elf}   IAP bootloader (ST-Link only)
#   firmware/dist/drdro-mainboard-factory.hex                merged image for blank boards
#   firmware/dist/SHA256SUMS.txt                             covers ALL release assets,
#                                                            firmware and software alike
#   software/dist/drdro-software.zip                         host software: the software/
#                                                            tree plus a prebuilt wheel
#
# Asset names are the contract with the host updater (software/dro/comms/release.py) —
# do not rename them without changing that module.
#
# The zip carries a PREBUILT WHEEL on purpose: installing from a source tree would need a
# build backend (hatchling) at install time, so an appliance update would depend on PyPI
# being reachable, not just GitHub. Installing the bundled wheel needs neither.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
SW="$ROOT/software"
SW_DIST="$SW/dist"
FW_DIST="$ROOT/firmware/dist"

if [ -n "${NEW_VERSION:-}" ]; then
  export FW_VERSION="v${NEW_VERSION}"
  echo "== release version ${FW_VERSION} (software + firmware) =="
fi

# ---- 1. firmware -----------------------------------------------------------
# Stages firmware/dist/ and writes a firmware-only SHA256SUMS.txt, which step 3 replaces
# with a combined manifest once the software artifacts exist.
"$ROOT/firmware/tools/build-release.sh"

# ---- 2. host software ------------------------------------------------------
echo "== building host software =="
rm -rf "$SW_DIST"
mkdir -p "$SW_DIST"

# Refresh the lock so drdro-software's self-version tracks the stamped pyproject, then
# build the wheel. `assets` in the root pyproject carries uv.lock into the release commit.
(cd "$SW" && uv lock && uv build --wheel --out-dir "$SW_DIST/wheel")

echo "== packing drdro-software.zip =="
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
PKG="$STAGE/software"
mkdir -p "$PKG"

# The source tree, minus everything an appliance must never receive: build caches, the
# dev venv, VCS metadata and test artifacts.
tar -C "$SW" \
    --exclude='.venv' \
    --exclude='dist' \
    --exclude='.git' \
    --exclude='.pytest_cache' \
    --exclude='__pycache__' \
    --exclude='*.pyc' \
    --exclude='.coverage' \
    --exclude='htmlcov' \
    -cf - . | tar -C "$PKG" -xf -

# The installable artifact lives beside the source it was built from.
mkdir -p "$PKG/dist"
cp "$SW_DIST"/wheel/*.whl "$PKG/dist/"

# Zipped with python rather than the `zip` binary: this script already needs python3 for
# make_factory.py, and `zip` is absent on plenty of dev machines (Git Bash on Windows,
# minimal containers). One less thing that only fails in CI.
python3 - "$STAGE" "$SW_DIST/drdro-software.zip" <<'PY'
import os, sys, zipfile

stage, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
    for root, _dirs, files in os.walk(stage):
        for name in sorted(files):
            full = os.path.join(root, name)
            # Store paths relative to the stage dir, so everything lands under software/.
            zf.write(full, os.path.relpath(full, stage).replace(os.sep, "/"))
print(f"wrote {out}")
PY
rm -rf "$SW_DIST/wheel"

# ---- 3. combined checksum manifest -----------------------------------------
# One manifest covering every asset, so the host can verify both halves of an update
# against a single file. Paths are basenames: assets are flat on the GitHub release.
echo "== writing combined SHA256SUMS.txt =="
(
  cd "$FW_DIST"
  # Drop the firmware-only manifest the firmware script just wrote before re-hashing,
  # or it would hash itself into its own replacement.
  rm -f SHA256SUMS.txt
  sha256sum ./* > SHA256SUMS.tmp
  (cd "$SW_DIST" && sha256sum ./*) >> SHA256SUMS.tmp
  mv SHA256SUMS.tmp SHA256SUMS.txt
)

echo "== done =="
ls -la "$FW_DIST" "$SW_DIST"
cat "$FW_DIST/SHA256SUMS.txt"
