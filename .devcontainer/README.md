# Dev container — drDRO mainboard V1.5

VS Code dev container for the firmware in `firmware/`: PlatformIO Core + the
`ststm32`/`stm32cube` toolchain (auto-fetched on first build), a host C compiler,
serial terminals, the GitHub CLI, `st-flash` (raw ST-Link flashing), network tools
for the W5500 bring-up, and the **Claude Code CLI** wired to your host login.

Mirrors the `drdro-firmware-f4` container, with the differences noted below.

## Use
Open the folder in VS Code → **Reopen in Container**. The first build warms the
cache (downloads the `arm-none-eabi` toolchain + framework — a few minutes, once;
cached in the `drdro-mainboard-platformio` volume thereafter).

> **This repo root is not a PlatformIO project.** The firmware lives in
> `firmware/`, next to the hardware design files, so every `pio` command needs
> `-d firmware`. `.vscode/tasks.json` wraps the common ones — **Ctrl+Shift+B**
> builds. If you want the PlatformIO *toolbar* (which keys off a `platformio.ini`
> in the workspace root), open the `firmware/` folder directly instead.

| Task    | Command |
|---------|---------|
| Build   | `pio run -d firmware/app` (bootloader: `-d firmware/bootloader`) |
| Flash   | `pio run -d firmware/app -t upload`  •  or `st-flash write firmware/app/.pio/build/drdro_mainboard_v15/firmware.bin 0x08020000 (app is Exec-linked; blank boards take firmware/factory.hex)` |
| Clean   | `pio run -d firmware/app -t clean` |
| Serial  | `tio /dev/ttyUSB0 -b 115200` — RS-485 debug console (needs a transceiver) |
| Ping    | `ping <board-ip>` — the container is on the host network |
| Claude  | `claude` — already signed in with your host account |

Host-side protocol unit tests: `pio test -d firmware/app -e native` (68 cases,
HAL/RTOS mocked under firmware/app/test/mocks/).

## Flashing / debugging (ST-Link over USB)
USB device passthrough is **Linux-host only**. The container runs `--privileged`
with host networking and bind-mounts all of `/dev`, so any USB-connected ST-Link
(and USB-serial adapter) is visible — including devices hot-plugged after the
container started.

The `/dev` bind mount only *exposes* the host's device nodes; it does not change
their permissions. So the udev rule must exist on the **host**. `49-stlink.rules`
in this folder is the one in use on this machine (already installed at
`/etc/udev/rules.d/49-stlink.rules`); on a fresh host:

```sh
sudo cp .devcontainer/49-stlink.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

PlatformIO's own `99-platformio-udev.rules` works equally well if you prefer it.

`setup.sh` reports ST-Link status at container-create time, so a missing rule or
an unplugged probe is obvious immediately rather than at first flash.

### WSL
The ST-Link must be attached to WSL from Windows first (admin PowerShell):

```powershell
usbipd list
usbipd attach --wsl --busid <BUSID>   # --auto-attach survives replug/probe reset
```

### OpenOCD transport quirk (already handled)
`ststm32@19.4.0` hardcodes `transport select hla_swd`, but the OpenOCD it pins
(0.12) dropped the legacy HLA driver, so a stock `-t upload` fails with
*"Debug adapter doesn't support 'hla_swd' transport"*. `firmware/support/
openocd_swd_transport.py` rewrites that one flag at upload time, so this is
transparent here — no need for the `stlink-tools` fallback. See that script's
header for why the platform pin is kept rather than bumped.

## Claude Code
`claude` is installed in the image (native standalone build, in `~/.local/bin`) and
the container **bind-mounts your host `~/.claude` and `~/.claude.json`**. That means
it shares the host's login (`~/.claude/.credentials.json`), settings, global
`CLAUDE.md`, and memory — no separate `/login` inside the container.

- Sharing works because the container's `vscode` user is uid 1000, matching the host
  user, so the mounted config files are owned correctly.
- `DISABLE_AUTOUPDATER=1` stops the container's claude from self-updating and
  rewriting the shared host config; update Claude on the host as usual.
- Because the config is *shared* (not copied), sessions/history from the container
  land in your host `~/.claude`.

## Ethernet bring-up notes
`--network=host` puts the container on the real LAN, so once the board has its DHCP
lease you can `ping` it and run `tcpdump` against the same segment — both installed.

Remember the hardware finding: **this W5500 only receives at 10 Mbps**, so a
10BASE-T-capable switch must be in the path. Full measurements in
`firmware/README.md`.

## How it's wired
- `Dockerfile` — base image + apt tools (compiler, serial, lrzsz, usbutils,
  **stlink-tools**, **iputils-ping/tcpdump**) and the **Claude Code** native install
  for the `vscode` user.
- `devcontainer.json` — `--privileged` + `--network=host`, `/dev` bind mount, host
  `~/.claude`/`~/.claude.json` bind mounts, and the PlatformIO extension pinned to
  the volume's CLI core.
- `setup.sh` (postCreate) — installs PlatformIO Core to `~/.platformio/penv`, warms
  the build **and pre-fetches `tool-openocd`** (otherwise first discovered at first
  flash), then reports claude / st-flash / ST-Link status.
- `~/.platformio` is the named volume `drdro-mainboard-platformio` — deliberately
  **separate** from the f4 project's `drdro-platformio` so the two cannot disturb
  each other's pinned packages.

## PlatformIO extension note
`platformio-ide.customPATH` points the extension at the CLI core installed in the
`~/.platformio` volume, and `useBuiltinPIOCore=false` stops it from downloading and
installing its *own* core on open — which otherwise races the `setup.sh` install and
triggers the "installing PlatformIO Core…" prompt on every container open.