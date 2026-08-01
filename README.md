# drDRO Mainboard V1.5

[![CI](https://github.com/bartei/drdro-mainboard/actions/workflows/ci.yml/badge.svg)](https://github.com/bartei/drdro-mainboard/actions/workflows/ci.yml)
[![Release](https://github.com/bartei/drdro-mainboard/actions/workflows/release.yml/badge.svg)](https://github.com/bartei/drdro-mainboard/actions/workflows/release.yml)

Hardware design **and** firmware for the drDRO 5-axis DRO / motion mainboard —
an STM32F411RET6 board that reads up to five quadrature encoder scales, drives a
step/dir servo axis, and talks to the host over RS-485 or Ethernet.

## The board

4-layer, 178 × 76 mm, 24 V supply (LMR33630 → 5 V → AMS1117 3.3 V). Designed in
EasyEDA Pro (`DRDRO Mainboard V1.5.eprj2`); mechanical model in
`drdro-mainboard.step`, enclosure models under `enclosure/`.

| Subsystem | Hardware |
|---|---|
| MCU | STM32F411RET6 @ 100 MHz (16 MHz HSE), SWD debug |
| Encoder inputs | 5 × DB9, differential RS-422 **or** single-ended TTL (AM26LV32E), hardware quadrature on TIM1–TIM5 |
| Host links | RS-485 (SP3485, USART1 @ 115200) and 10/100 Ethernet (WIZnet W5500 on SPI2 — see the 10BASE-T caveat in `firmware/README.md`) |
| Motor outputs | 3 × STEP/DIR + shared ENA through a ULN2003 (M1 is the servo axis) |
| Digital inputs | 6 × opto-isolated (TLP2309) |
| Analog outputs | 2 × 0–10 V (GP8403 12-bit I²C DAC) |
| Expansion | I²C bus exported on every encoder DB9 |

Design docs, pin map and board status live in `docs/` (`STATUS.md` is the
authoritative hardware state; `mcu_pinmap.md` the pin assignment). `h723_variant/`
holds the design study for a future STM32H723 sibling board.

## The firmware

Complete product firmware under `firmware/` — see
[`firmware/README.md`](firmware/README.md) for the full feature list, protocol
description and build instructions. Highlights:

- FreeRTOS application + dual-bank **IAP bootloader** (A/B storage banks,
  copy-on-activate, power-loss-safe settings, rollback).
- One line protocol (`key=value` + CRC framing) served identically over
  **RS-485 and TCP** (default port 5555, ~250 Hz request rate).
- **Firmware updates over Ethernet** (the running app writes the inactive bank)
  or over RS-485 (bootloader YMODEM — also the recovery path), both driven by
  `firmware/tools/dro_update.py`.
- Host-testable protocol core: `pio test -d firmware/app -e native`.

## Building / flashing

```sh
pip install platformio
pio run  -d firmware/app                 # application (Exec region 0x08020000)
pio run  -d firmware/bootloader          # IAP bootloader (sector 0)
pio test -d firmware/app -e native       # protocol unit tests
firmware/tools/build-release.sh          # firmware/dist/ + merged factory image
```

A blank board is flashed once over SWD with `drdro-mainboard-factory.hex`
(bootloader + app merged). After that, updates need no debugger:

```sh
firmware/tools/dro_update.py --net <board-ip> drdro-mainboard-app.bin   # Ethernet
firmware/tools/dro_update.py /dev/ttyUSB0    drdro-mainboard-app.bin   # RS-485
```

The repo ships a devcontainer (`.devcontainer/`) with the full toolchain,
ST-Link/USB passthrough and host networking — see `.devcontainer/README.md`.

## Releases

Versioning and releases are automated with
[python-semantic-release](https://python-semantic-release.readthedocs.io/) from
[Conventional Commits](https://www.conventionalcommits.org/): every push to
`master` runs the test suite and, if there are releasable changes, tags
`vX.Y.Z`, regenerates [`CHANGELOG.md`](CHANGELOG.md), publishes GitHub release
notes and attaches the firmware assets:

| asset | purpose |
|---|---|
| `drdro-mainboard-app.bin` | application image — feed to `dro_update.py` (automated updates) |
| `drdro-mainboard-app.{hex,elf}` | same image for programmers / debugging |
| `drdro-mainboard-bootloader.{bin,hex,elf}` | IAP bootloader (SWD only) |
| `drdro-mainboard-factory.hex` | merged bootloader + app — first flash of a blank board |
| `SHA256SUMS.txt` | asset checksums |

Pushes to a `dev` branch publish `vX.Y.Z-beta.N` prereleases.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). The firmware descends from the
[drdro-firmware-f4](https://github.com/bartei/drdro-firmware-f4) baseline.
