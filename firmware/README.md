# drDRO mainboard V1.5 — firmware

Full firmware for the **STM32F411RET6** on the DRDRO Mainboard V1.5 (the board in
this repo): PlatformIO + STM32Cube HAL + FreeRTOS. Ported from the proven
`drdro-firmware-f4` baseline (the previous, RS-485-only board) and extended with
this board's hardware: 5 encoder scales, W5500 Ethernet (TCP CLI + network
firmware updates), GP8403 analog outputs, and 6 opto-isolated inputs.

## Layout — two PlatformIO projects + a shared contract

```
firmware/
├── app/          the application  (Exec region 0x08020000, FreeRTOS, all features)
├── bootloader/   IAP bootloader   (sector 0 @ 0x08000000, polled, self-contained)
├── shared/       app<->bootloader contract: Bootloader.h (flash map + handshake),
│                 Settings.h ("DRO2" persisted layout), BlinkCode.h, BoardPins.h
├── tools/        dro_update.py (serial + --net updater), make_factory.py,
│                 build-release.sh
└── reference/    TEMPORARY copy of drdro-firmware-f4 (gitignored, never commit)
```

## Build / flash / test

```sh
pio run  -d firmware/app                    # build the app
pio run  -d firmware/bootloader             # build the bootloader
pio run  -d firmware/app -t upload          # flash the app over ST-Link (SWD)
pio test -d firmware/app -e native          # host-side protocol unit tests (68)
firmware/tools/build-release.sh             # dist/ artifacts + merged factory.hex
```

A blank board gets the merged `factory.hex` (bootloader + app in one image);
after that, updates go through the dual-bank IAP cycle — no ST-Link needed:

```sh
# over Ethernet (the running app writes the inactive bank; ~30 s):
firmware/tools/dro_update.py --net <board-ip> firmware/app/.pio/build/drdro_mainboard_v15/firmware.bin

# over RS-485 (bootloader YMODEM; also the recovery path):
firmware/tools/dro_update.py /dev/ttyUSB0 firmware/app/.pio/build/drdro_mainboard_v15/firmware.bin
```

## Features / interfaces

- **CLI line protocol** — identical wire format on both transports
  (`cmd [args]\r` in; `key=value` lines + `crc=HH` + blank line out; optional
  `*HH` request checksum; empty line repeats the last command):
  - **RS-485**: USART1 @ 115200 8N1, DE on PA11 (asserted per response, released
    on the TC interrupt).
  - **TCP**: port `net.port` (default **5555**), one client at a time. DHCP by
    default; static IP via `net.dhcp=0` + `net.cfg.*` (applied at next boot).
- **Commands**: `sta get set settings save load bank rollback version help
  update reset dout fw.begin fw.status fw.commit fw.abort`
- **Registry**: `scales.pos/speed/num/den/sync/filt/dir` (×5),
  `servo.max/acc/jog/idx/mode/pos/speed/tgt`, `din.state/cnt/deb`,
  `aout.raw` (×2, 0..4095 = 0..10 V), `net.*`, `diag.cycles/interval/cycmax/uptime`
  (cycle counts are DWT ticks @ 100 MHz = 10 ns; `set diag.cycmax 0` re-arms the
  worst-case hold; uptime as `<days>d<HH>:<MM>:<SS>`).
- **Encoders**: 5 hardware-quadrature scales on TIM1..TIM5. The AM26LV32E
  receiver inversion is fixed in `Scales.c` (IC1 polarity); `scales.dir` is the
  user-level flip.
- **Motion**: single step/dir servo axis (M1, TIM9 100 kHz RAM-resident ISR —
  survives flash writes); shared ENA; M2/M3 pins exercisable via `dout`.
- **Updates**: dual-bank copy-on-activate (see `shared/Bootloader.h` for the
  flash map). Ethernet updates stream into the inactive bank on port
  `net.port+1` and only `fw.commit` (full-size + CRC32 + vector checks) can
  activate it; the UART-only bootloader (YMODEM) is the recovery path.
- **LED** (PA12): blink codes — 1 app running / 2 bootloader / 3 network down /
  4 W5500 error / 5 flash fault (`shared/BlinkCode.h`).

## Hardware caveat

The W5500 on this board only *receives* at **10BASE-T** (verified per-mode on
hardware; see `app/README.bringup.md` for the measurement). `Net.c` forces
10M-half first and a 10 Mbps-capable switch is required.

## Baseline / history

- `app/README.bringup.md` — the original Ethernet bring-up notes (clock/SPI/
  DHCP verification, PHY-mode measurements, MACRAW self-test).
- The `drdro-firmware-f4` project is the design baseline; its docs
  (`protocol_design.md`, `dualbank_design.md`) still describe the protocol and
  bank machinery authoritatively. Differences here: 5 scales (registry arrays
  are 5 wide), settings magic **DRO2** (fresh payload, same frozen core), DE
  pin handling, TCP transport, `dout`/`fw.*`/`din.*`/`aout.*`/`net.*`, and a
  version-stamped bootloader (`BL_VERSION`).
