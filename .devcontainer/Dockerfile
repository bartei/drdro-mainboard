# drDRO mainboard V1.5 dev environment — PlatformIO + STM32F4 + serial/USB/network tooling.
FROM mcr.microsoft.com/devcontainers/python:1-3.12-bookworm

# System tools:
#   build-essential  - host C compiler (native unit tests, when they are added)
#   tio / minicom    - serial terminals for the RS-485 debug console (115200 8N1)
#   lrzsz            - X/Y/ZMODEM (sz/rz), for a future UART bootloader
#   usbutils / udev  - lsusb + device rules for ST-Link
#   stlink-tools     - st-flash / st-info / st-util, a second flashing path
#   srecord          - hex/bin manipulation
#   iputils-ping     - verify the board's DHCP lease is really reachable
#   tcpdump          - inspect Ethernet traffic; this board's W5500 link is the
#                      active bring-up area, and --network=host means captures
#                      see the real LAN
# Drop the base image's yarn apt source (broken GPG key, unneeded here) so
# `apt-get update` doesn't fail, then install our tools.
RUN grep -rlZ 'dl.yarnpkg.com' /etc/apt/ 2>/dev/null | xargs -0r rm -f; \
    apt-get update && export DEBIAN_FRONTEND=noninteractive \
 && apt-get install -y --no-install-recommends \
      build-essential \
      tio minicom lrzsz \
      usbutils udev \
      stlink-tools \
      srecord \
      iputils-ping \
      tcpdump \
 && apt-get clean && rm -rf /var/lib/apt/lists/*

# Claude Code CLI (native standalone install — no Node required). Installed as the
# `vscode` user so it lands in /home/vscode/.local/bin. The host's ~/.claude and
# ~/.claude.json are bind-mounted at runtime (see devcontainer.json), so this
# in-container binary runs against the host's login/config. DISABLE_AUTOUPDATER
# (containerEnv) keeps it from mutating the shared host config.
USER vscode
RUN curl -fsSL https://claude.ai/install.sh | HOME=/home/vscode bash
USER root

# PlatformIO Core lives in the ~/.platformio volume (installed by setup.sh); the
# Claude CLI in ~/.local/bin. Expose both.
ENV PATH="/home/vscode/.platformio/penv/bin:/home/vscode/.local/bin:${PATH}"

# ENV alone is not enough: VS Code's integrated terminal starts a LOGIN shell,
# which re-sources /etc/profile and rebuilds PATH from scratch — dropping the
# penv entry above, so `pio` would not be found in the terminal (verified on this
# base image). Re-add it via profile.d so login shells keep it too.
# (~/.local/bin, where the Claude CLI lives, is already added by the base image's
# own profile scripts — only the PlatformIO entry needs restoring here.)
RUN printf '%s\n' \
      '# drDRO: PlatformIO Core lives in the ~/.platformio volume.' \
      'case ":$PATH:" in' \
      '  *":/home/vscode/.platformio/penv/bin:"*) ;;' \
      '  *) PATH="/home/vscode/.platformio/penv/bin:$PATH"; export PATH ;;' \
      'esac' \
      > /etc/profile.d/10-drdro-path.sh \
 && chmod 0644 /etc/profile.d/10-drdro-path.sh