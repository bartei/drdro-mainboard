#!/usr/bin/env bash
#
# Dev container provisioning: install PlatformIO Core into the ~/.platformio volume
# and warm the build cache (platform + stm32cube framework + arm-none-eabi toolchain
# + tool-openocd).
#
# NOTE this repo is not itself a PlatformIO project: the firmware lives in
# firmware/, alongside the hardware design files. Hence the `-d firmware`.
#
set -euo pipefail

PIO_PROJECT_DIR="firmware/app"
PIO_ENV="drdro_mainboard_v15"

# The ~/.platformio volume mounts as root the first time — make it ours.
sudo mkdir -p "$HOME/.platformio"
sudo chown -R "$(id -u):$(id -g)" "$HOME/.platformio"

# Install PlatformIO Core (idempotent). The official installer creates the
# ~/.platformio/penv layout that the PlatformIO IDE extension expects, so the
# CLI and the extension share one core (see useBuiltinPIOCore=false).
if [ ! -x "$HOME/.platformio/penv/bin/pio" ]; then
  echo "[setup] Installing PlatformIO Core..."
  curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o /tmp/get-platformio.py
  python3 /tmp/get-platformio.py
  rm -f /tmp/get-platformio.py
fi

export PATH="$HOME/.platformio/penv/bin:$PATH"
echo "[setup] $(pio --version)"

# Warm the cache and verify the toolchain builds the project. One-time, ~minutes;
# non-fatal so the container still comes up if offline.
echo "[setup] Warming build (downloads the arm toolchain on first run)..."
pio run -d "$PIO_PROJECT_DIR" -e "$PIO_ENV" \
  || echo "[setup] warm build skipped/failed — run 'pio run -d firmware/app' manually once online."

# tool-openocd is only fetched on first upload, which is an annoying time to
# discover you are offline. Pull it now so flashing works immediately.
echo "[setup] Fetching tool-openocd (uploader)..."
pio pkg install -g -t platformio/tool-openocd >/dev/null 2>&1 \
  || echo "[setup] tool-openocd fetch skipped — it will be pulled on first upload."

# GitHub push access depends on the host ~/.ssh bind mount — surface it here
# rather than at first push. (BatchMode: never hang on a passphrase prompt.)
if [ -d "$HOME/.ssh" ] && ls "$HOME/.ssh"/id_* >/dev/null 2>&1; then
  if ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -T git@github.com 2>&1 | grep -q "successfully authenticated"; then
    echo "[setup] github:   SSH key works (push enabled)"
  else
    echo "[setup] github:   ~/.ssh mounted but auth failed — passphrase-protected key? run ssh-add on the HOST (agent is forwarded)"
  fi
else
  echo "[setup] github:   no ~/.ssh key found — check the devcontainer ~/.ssh bind mount"
fi

# Report the extra tooling so a broken share/install is obvious at create time.
echo "[setup] claude:   $(command -v claude >/dev/null && claude --version 2>/dev/null || echo 'NOT FOUND')"
if [ -r "$HOME/.claude/.credentials.json" ]; then
  echo "[setup] claude:   host login shared (~/.claude/.credentials.json present)"
else
  echo "[setup] claude:   ~/.claude/.credentials.json not found — run 'claude' and /login once, or check the host bind mount."
fi
echo "[setup] st-flash: $(st-flash --version 2>&1 | head -n1 || echo 'NOT FOUND')"

# ST-Link visibility depends on a HOST udev rule (the /dev bind mount just exposes
# whatever permissions the host set). Surface it here rather than at first flash.
if lsusb 2>/dev/null | grep -qi '0483:37'; then
  echo "[setup] st-link:  probe detected on USB"
  if st-info --probe >/dev/null 2>&1; then
    echo "[setup] st-link:  accessible (permissions OK)"
  else
    echo "[setup] st-link:  DETECTED BUT NOT ACCESSIBLE — install the udev rule on the HOST (see .devcontainer/README.md)"
  fi
else
  echo "[setup] st-link:  not connected (attach it, or on WSL run 'usbipd attach --wsl --busid <id>')"
fi

echo "[setup] Done. Build: pio run -d firmware/app | Flash: pio run -d firmware/app -t upload"
