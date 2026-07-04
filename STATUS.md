# drDRO mainboard — status & resume point

**Last reviewed:** 2026-07-04 · netlist `Netlist_Schematic1_2026-07-04.net`
**This file is the authoritative current status.** Per-subsystem detail lives in
the design docs (index at bottom). The older `*_todo.md` files are historical
working notes from the design evolution — this file supersedes them.

## What the board is
5-axis DRO mainboard. STM32F411RET6 (LQFP64) + 5× universal encoder inputs
(differential RS-422 *or* single-ended TTL, no mux) + 3× stepper STEP/DIR/ENA
outputs (via ULN2003) + RS-485 + W5500 SPI Ethernet + 6× opto-isolated inputs
+ 24V→5V (LMR33630 buck) →3.3V (AMS1117) power. Fab: JLCPCB.

## Subsystem status — all verified against the latest netlist
| Subsystem | Status |
|---|---|
| Power: 24V→5V buck (LMR33630ADDAR) | ✅ FB trim 4.82–5.20V, all rails/EP correct |
| Power: 5V→3.3V (AMS1117) + MCU power | ✅ all 5 VDD/VSS pairs wired, VCAP=2.2µF |
| MCU pin assignment | ✅ encoders on TIM1–5, all peripherals on valid AF pins |
| Reset + BOOT0 circuits | ✅ pull-up + steering diodes + buttons |
| Encoder inputs (universal, no mux) | ✅ 2.2k bias, no term, receivers→MCU, J1–J5→ENC1–5 |
| Motor outputs (3× STEP/DIR/ENA) | ✅ PC0–3/PC10–12→ULN2003→CN6/CN7, M3_DIR off PC13 |
| RS-485 (SIT3088, GPIO-DE) | ✅ PA11 DE, 680Ω bias, 120Ω term — 10Mbps-ready |
| W5500 Ethernet | ✅ SPI/bias/PMODE/magnetics/LEDs/crystal all correct |

## OPEN — before fab (schematic/wiring)
- [ ] **I²C expansion bus** (SCL/SDA on DB9 pins 5/9 of J1–J5): add **pull-ups**
      (needed for the bus to work) + **ESD** footprints. Currently neither.
      Decide 3.3V vs 5V level; add a bus buffer if long cables.
- [ ] **SPI NSS**: optional ~10k pull-up on W5500 SCSn→3V3 (clean deselect at boot).
- [ ] **RESET**: optional 100nF to GND (ST-recommended, not required).
- [ ] **W5500 crystal**: optional 1MΩ feedback (XI–XO) + 0Ω series on XO per
      WIZnet ref Fig 3 (usually not needed — internal feedback exists).
- [ ] **Connector grouping** (optional): motor 2 is split across CN6 (M2_STEP)
      and CN7 (M2_DIR). Regroup per-motor if desired. CN6/CN7 are signal-only.

## OPEN — PCB layout
- [ ] Local 100nF decoupling per logic IC (digital 3V3 now ~12 caps — verify per-IC).
- [ ] U5 SIT3088 DFN **exposed pad → GND**; U1 LMR33630 thermal pad → GND via array.
- [ ] MCU crystal X1 (12pF) load caps vs its CL — verify. (W5500 X3 18pF = OK.)
- [ ] Keep buck hot loop tight; FB trace away from SW/inductor; W5500 diff pairs.

## FIRMWARE notes
- **SPI NSS**: drive PB12 as **GPIO software CS** (SPI2 SSM=1/SSI=1), *not* HW-NSS —
  W5500 needs SCSn to frame each transaction (VDM mode).
- **Encoders**: ENC5 must use **TIM5 (AF2)**, not TIM2 (PA0/PA1 are both).
- **RS-485**: assert PA11 (DE) before TX, deassert in USART **TC** interrupt
  (F411 has no hardware DEM). USART1 on APB2 → 10Mbps clean.
- **ISO_IN1–6**: enable STM32 internal pull-ups (TLP2309 open-collector outputs).
- **Motor STEP**: no free timer channels (TIM1–5 = encoders) → GPIO/DMA step gen.

## BRING-UP checklist
- [ ] 5V rail correct across 0–2A load; efficiency/thermal at 2A.
- [ ] 3.3V rail; MCU boots; SWD connects.
- [ ] Each encoder counts (differential + single-ended scale).
- [ ] RS-485 loopback at target baud; W5500 link + ping; each motor STEP/DIR.

## Document index
- `STATUS.md` — this file (authoritative current state).
- `mcu_pinmap.md` — full verified STM32F411 pin map + AF audit.
- `power_supply_design.md` — buck + RS-485 detail, decisions, equations, review rounds.
- `encoder_input_design.md` — universal encoder front-end design + rationale.
- `board_review_todo.md` — full-board review findings (most current todo).
- `power_supply_todo.md`, `encoder_input_todo.md` — historical working notes.
