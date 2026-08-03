#!/usr/bin/env bash
#
# Build release artifacts for the drDRO mainboard V1.5 firmware:
#   dist/drdro-mainboard-app.{bin,hex,elf}         (Exec-linked application)
#   dist/drdro-mainboard-bootloader.{bin,hex,elf}  (sector-0 IAP bootloader)
#   dist/drdro-mainboard-factory.hex               (merged, flash on blank boards)
#   dist/SHA256SUMS.txt
#
# Version stamping: python-semantic-release invokes this as build_command with
# NEW_VERSION exported — bake it into the binaries as FW_VERSION (the `version`
# CLI command). FW_VERSION can also be pinned directly; otherwise the support
# scripts fall back to `git describe`. Asset names differ from the
# drdro-firmware-f4 baseline (drdro-app.bin etc.) on purpose: this board's
# images must never be pushed to the old board by an updater matching on name.
#
# Run from the repo root or firmware/: paths are resolved relative to this script.
#
# NOTE: this builds the FIRMWARE half only. Releases go through tools/build-release.sh at
# the repo root, which calls this script, then builds the host software and REPLACES the
# firmware-only dist/SHA256SUMS.txt written here with a combined manifest covering every
# release asset. Running this script directly (a firmware-only local build) is still fine
# and leaves the firmware-only manifest in place.
set -euo pipefail

if [ -n "${NEW_VERSION:-}" ]; then
  export FW_VERSION="v${NEW_VERSION}"
  echo "== FW_VERSION=${FW_VERSION} (from semantic-release) =="
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="$(dirname "$HERE")"                       # firmware/
DIST="$FW/dist"

APP_ENV="drdro_mainboard_v15"
APP_BUILD="$FW/app/.pio/build/$APP_ENV"
BL_BUILD="$FW/bootloader/.pio/build/bootloader"

echo "== building app =="
pio run -d "$FW/app" -e "$APP_ENV"
echo "== building bootloader =="
pio run -d "$FW/bootloader"

echo "== merging factory image =="
python3 "$HERE/make_factory.py" "$FW/factory.hex" \
  "$BL_BUILD/firmware.hex" "$APP_BUILD/firmware.hex"

echo "== staging dist/ =="
rm -rf "$DIST"
mkdir -p "$DIST"
for ext in bin hex elf; do
  cp "$APP_BUILD/firmware.$ext" "$DIST/drdro-mainboard-app.$ext"
  cp "$BL_BUILD/firmware.$ext"  "$DIST/drdro-mainboard-bootloader.$ext"
done
cp "$FW/factory.hex" "$DIST/drdro-mainboard-factory.hex"

(cd "$DIST" && sha256sum ./* > SHA256SUMS.txt)
echo "== done =="
ls -la "$DIST"
