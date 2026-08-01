# drDRO mainboard V1.5 — firmware

Firmware for the **STM32F411RET6** on the DRDRO Mainboard V1.5 (the board in this
repo). Built with PlatformIO + STM32Cube HAL + FreeRTOS.

The tested/working firmware for the *previous* board lives in the sibling project
`../../drdro-firmware-f4` and is the baseline this project was started from. That
project targets a **different MCU and pin map** — see "Deltas from the baseline".

## Current scope: Ethernet bring-up — ✅ VERIFIED ON HARDWARE

This image is deliberately minimal. It exists to prove the two things that must
work before anything else is worth porting:

1. the MCU boots and runs at the right clock (blinking status LED), and
2. the **W5500 Ethernet controller comes up and obtains an address via DHCP**.

Both confirmed on the first board (2026-07-31): 100 MHz from the 16 MHz crystal,
SPI2 reads *and* writes to the W5500 verified, DHCP lease obtained, and the board
answers ICMP from another host on the LAN (~1.2 ms). See the 10 Mbps caveat below.

The motion stack (encoders, step generation), the RS-485 line protocol, the
GP8403 analog outputs, the opto-isolated inputs and the IAP bootloader from the
baseline are **not ported yet**.

## Build / flash

PlatformIO Core is required (`pip install platformio`).

```sh
pio run                 # build
pio run -t upload       # flash over ST-Link (SWD)
pio run -t clean
```

Artifacts land in `.pio/build/drdro_mainboard_v15/firmware.{elf,bin,hex}`.

Debug output goes out the **RS-485 port at 115200 8N1** (USART1, DE on PA11) and
reports the MAC, link transitions and the DHCP-assigned address:

```
drDRO mainboard V1.5 (STM32F411RET6) <git describe>
SYSCLK 100000000 Hz
W5500: ready, MAC 02:xx:xx:xx:xx:xx
ETH: link up
DHCP: discovering...
DHCP assigned: IP 192.168.1.42/255.255.255.0 GW 192.168.1.1 DNS 192.168.1.1
```

## ⚠ Ethernet: this W5500 only works at 10 Mbps

**Verified on hardware 2026-07-31.** The board obtains a DHCP lease and answers
ping only when the PHY is forced to **10BASE-T**. At 100BASE-TX the link comes up
normally but **not one frame is ever received**.

Measured with a MACRAW sniff (receives every frame on the wire, no MAC filter),
same board, same cable, same switch — only the PHY speed differed:

| PHY mode | link | bytes received in 4 s |
|---|---|---|
| **10M half** | up | **1726** |
| auto-neg (settles at 100M) | up | **0** |
| 100M full | up | **0** |
| 100M half | up | **0** |
| **10M full** | up | **1950** |

`Sn_RX_RSR` only counts CRC-good frames, so a PHY that cannot decode 100BASE-TX
yields exactly zero rather than "some errors" — which is why this looked at first
like "no DHCP server on the network".

Consequences:

- `src/Net.c` tries **10M half first** and stays there once DHCP succeeds.
- **A 10 Mbps-capable switch is required.** Many modern gigabit switches have no
  10BASE-T support at all; with such a switch both 10M modes fail to link and
  there is no working configuration. That was the original bring-up failure.
- This is **not** a fault of this PCB: a broken RXP/RXN pair or bad magnetics
  would fail at 10 Mbps too. The same behaviour was previously seen on unrelated
  W5500 breakout boards.
- Set `-D NET_SELFTEST=1` to re-run the per-mode measurement on new hardware
  (adds ~40 s to boot). Raw W5500 registers are mirrored into the `netDiag`
  struct for inspection over SWD.

## Status LED (LED3, PA12)

LED3 is wired **active low** (cathode to PA12, anode via R13 to 3V3). One LED has
to carry the whole bring-up state, so each state has its own pattern:

| Pattern (on/off ms) | Meaning |
|---|---|
| 500 / 500 — even 1 Hz blink | booting, W5500 not up yet |
| 800 / 200 — mostly on | **W5500 not responding** (VERSIONR != 0x04): check SPI2 / CS |
| 100 / 100 — fast flicker | chip alive, **Ethernet link down** (cable/switch) |
| 100 / 400 — short pulses | link up, DHCP in progress |
| 100 / 900 — one blip per second | DHCP failed, backing off before retry |
| 50 / 1950 — one brief flash every 2 s | **address leased — healthy** |

## Deltas from the `drdro-firmware-f4` baseline

These are the changes that were required for this board. Pin assignments come
from the board netlist (`Netlist_Schematic1_2026-07-10.net`), cross-checked
against `../docs/mcu_pinmap.md` — **not** from the baseline project.

| | baseline (`drdro-firmware-f4`) | this project |
|---|---|---|
| MCU | STM32F411CEU6, LQFP-48 | **STM32F411RET6, LQFP-64** (`genericSTM32F411RE`) |
| HSE crystal | 8 MHz | **16 MHz** (X1 = X322516MLB4SI) |
| PLLM | 4 | **8** (keeps SYSCLK at 100 MHz) |
| Status LED | PB12 | **PA12**, active low |
| Link script | app @ `0x08020000` (IAP) | framework default @ `0x08000000` |
| Ethernet | none | **W5500 on SPI2** + DHCP |

The crystal change is the subtle one: PB12 on this board is the W5500 chip
select, and leaving `PLLM = 4` with a 16 MHz crystal would request a 200 MHz
SYSCLK. `src/main.c` carries `_Static_assert`s over the whole clock tree so a
wrong `HSE_VALUE` or PLL divider is a **build error**, not a silent bring-up
failure.

## Layout

```
platformio.ini            build config; HSE_VALUE and the ioLibrary chip select
include/BoardPins.h       authoritative V1.5 pin map (single source of truth)
include/Net.h             W5500/DHCP bring-up API
src/main.c                clock tree (+ compile-time checks), blink task
src/Net.c                 SPI glue for the ioLibrary, W5500 reset, DHCP task
src/spi.c                 SPI2 master, software CS (W5500 needs VDM framing)
src/usart.c               USART1 + RS-485 DE, blocking debug print
src/gpio.c                LED + W5500 nRST/nINT/CS
lib/FreeRTOS/             vendored, unchanged from the baseline
lib/ioLibrary/            vendored WIZnet driver subset (W5500 + DHCP)
support/                  hard-float, git version stamp, .hex emit
```

### Notes carried over from the hardware review

- **SCSn must be a software GPIO** (PB12), not hardware NSS: the W5500 frames a
  whole variable-data-mode transaction with SCSn. See `../docs/STATUS.md`.
- **RS-485 DE** is a plain GPIO — the F411 has no hardware driver-enable, so DE is
  asserted before TX and released once TC is set.
- When the encoders are ported: **ENC5 must use TIM5 (AF2)**, not TIM2 — PA0/PA1
  are valid for both, but TIM2 belongs to ENC2.
- When the inputs are ported: enable **internal pull-ups** on ISO_IN1–6, the
  TLP2309 outputs are open-collector.
