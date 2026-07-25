# drDRO mainboard — status & resume point

**Last reviewed:** 2026-07-11 · **final pre-order review** — netlist
`Netlist_Schematic1_2026-07-10.net` (**189 comp / 138 nets, zero dangling, all
rails/EP resolved**) + Gerber `Gerber_PCB1_2026-07-11.zip` (4-layer,
JLC04161H-7628 impedance stackup, 100 Ω diff pairs) + project `.epro2`. All three
cross-check as the same current design. (The `.webp` previews inside the `.epro2`
are stale cached thumbnails of an old V1.4/V1.5 mux-era layout — **ignore them**.)
**This file is the authoritative current status.** Per-subsystem detail lives in
the design docs (index at bottom). Most `*_todo.md` files are historical working
notes that this file supersedes — **except `encoder_input_todo.md`, which is the
live tracker for the encoder work** (see OPEN below).

**Δ since 2026-07-04:** the encoder front-end restructure is now **partly done** —
the 3 shared quad receivers became **5 receivers, one AM26LV32E per DB9**
(U7=J1, U8=J2, U9=J3, U10=J4, U11=J5), each A/B only. **Index-Z is still NOT
wired** (DB9 pin 6 free on all 5; spare receiver channels 3/4 tied off; Z GPIOs
unused). SPI NSS pull-up now added (R72). No critical defects found.

## What the board is
5-axis DRO mainboard. STM32F411RET6 (LQFP64) + 5× universal encoder inputs
(differential RS-422 *or* single-ended TTL, no mux) + 3× stepper STEP/DIR/ENA
outputs (via ULN2003) + RS-485 + W5500 SPI Ethernet + 6× opto-isolated inputs
+ 24V→5V (LMR33630 buck) →3.3V (AMS1117) power. Fab: JLCPCB.
Encoder front-end is currently **A/B-only across 3 shared quad receivers**
(U7/U8/U9); the agreed **one-receiver-per-DB9 (5 chips) + index-Z** restructure
is still to be implemented — see OPEN and `encoder_input_todo.md`.

## Subsystem status — all verified against the latest netlist
| Subsystem | Status |
|---|---|
| Power: 24V→5V buck (LMR33630ADDAR) | ✅ FB trim 4.82–5.20V, all rails/EP correct |
| Power: 5V→3.3V (AMS1117) + MCU power | ✅ all 5 VDD/VSS pairs wired, VCAP=2.2µF |
| MCU pin assignment | ✅ encoders on TIM1–5, all peripherals on valid AF pins |
| Reset + BOOT0 circuits | ✅ pull-up + steering diodes + buttons |
| Encoder inputs (universal, no mux) | 🚧 **5-chip-per-DB9 done** (U7=J1…U11=J5, AM26LV32EIDR, 2.2k bias, no term, receivers→MCU on TIM1–5); spare recv ch tied off; **index-Z still NOT wired** (DB9 pin6 free, Z GPIOs unused) |
| Motor outputs (3× STEP/DIR/ENA) | ✅ PC0–3/PC10–12→ULN2003→CN6/CN7, M3_DIR off PC13 |
| RS-485 (GPIO-DE) | ✅ PA11 DE, 680Ω bias, 120Ω term — 10Mbps-ready. **U5 = SP3485EN-L/TR (C8963, SOIC-8) now PLACED** in Gerber 2026-07-11 (pinout verified: RO→UART_RX, RE̅+DE→UART_DE, DI→UART_TX, A/B, VCC=3V3). SOIC-8 has no EP → old "EP→GND" item moot |
| W5500 Ethernet | ✅ SPI/bias/PMODE/magnetics/LEDs/crystal all correct |
| Analog outputs (2× 0–10 V, VFD ref) | ✅ **in netlist + reviewed 2026-07-10** — U20 GP8403 dual I²C DAC, VCC=VIN(24 V), EP→GND, on shared I²C1 (PB8/PB9, addr 0x58); VOUT0/1→AO0/AO1 (1 µF + SMAJ12A + 47 Ω)→CN9; 4–20 mA dropped. See `analog_output_design.md` |

## OPEN — before fab (schematic/wiring)
- [x] 🆕 **2× 0–10 V analog outputs (VFD speed ref) — DONE in netlist + reviewed
      2026-07-10.** U20 **GP8403** dual 12-bit I²C DAC (**C3152007**), VCC=VIN(24 V),
      EP→GND, on the **shared I²C1** bus (PB8/PB9, **addr 0x58**), pull-ups R14/R74
      reused. VOUT0→AO0 / VOUT1→AO1, each 1 µF + SMAJ12A TVS (C113957) + 47 Ω → CN9
      (AO0/AO1/GND/GND). 0–10 V only — 4–20 mA dropped. Caps 1 µF per official
      datasheet. Detail: `analog_output_design.md` / `analog_output_todo.md`.
      Open (optional): local 0.1 µF + RC on VCC (24 V-bus noise); reserve I²C 0x58.
- [x] **Encoder split → one AM26LV32E per DB9 (5 chips).** DONE in
      `Netlist_PCB1_2026-07-09.net`: U7=J1, U8=J2, U9=J3, U10=J4, U11=J5, each
      A/B, per-line 2.2k pull-up + 2.2k/2.2k bias, receivers enabled (pin4=3V3),
      spare ch3/4 inputs tied to defined levels, outputs on TIM1–5. Verified.
- [x] ~~Index-Z~~ **DESCOPED** (user decision 2026-07-09): Z inputs will never be
      connected. DB9 pin 6 stays free; spare receiver channels remain tied off. No
      work owed. Encoder front-end is final at 5×AM26LV32E, A/B only.
- [x] ~~CN1~~ NON-ISSUE (user 2026-07-09): CN1 is the pluggable **mating plug**
      carried in the BOM for the WJ15EDG headers — intentionally unwired. Ignore.
- [x] **I²C expansion bus pull-ups** added (R14/R74 = 4.7k to 3V3 on SDA/SCL).
      DONE. Optional still: ESD footprints on the DB9-routed SCL/SDA if long cables.
- [x] **Optos — OK as designed** (decision 2026-07-09): TLP2309s wired correctly;
      keep board-5V sinking inputs. Moving to an isolated field domain later is just
      a pin swap (LED-anode resistor → field supply), so no rework now.
- [ ] **Add ESD/surge protection on IN1–6** (requested): per line at CN4/CN5,
      bidirectional TVS to GND = **SMAJ6.0CA (JLCPCB C466511)**, 6V standoff (clears
      the 0–5V operating range); optional ~100Ω series R to the opto for extra LED
      protection.
- [ ] 🔧 **Replace D1 TVS** → **SMCJ30A (JLCPCB C408374 or C135160)**, unidirectional,
      30V standoff, SMC/DO-214AB (same footprint as D1). Clamps earlier (starts ~33V)
      and lower than the old SMDJ36CA so it actually protects the LMR33630
      (VC ~35–40V at realistic surge currents, < the 42V abs-max). D2 (SL10100) fine.
- [x] **SPI NSS pull-up**: ~10k on W5500 SCSn→3V3 now present (R72). DONE.
- [ ] **RESET**: optional 100nF to GND (ST-recommended, not required).
- [ ] **W5500 crystal**: optional 1MΩ feedback (XI–XO) + 0Ω series on XO per
      WIZnet ref Fig 3 (usually not needed — internal feedback exists).
- [ ] **Connector grouping** (optional): motor 2 is split across CN6 (M2_STEP)
      and CN7 (M2_DIR). Regroup per-motor if desired. CN6/CN7 are signal-only.

## PCB LAYOUT — FINAL pre-order review 2026-07-11 (`Gerber_PCB1_2026-07-11.zip`)
Reviewed from Gerber + drill + flying-probe + `.epru` (no raw-Gerber renderer here;
the `.epro2` `.webp` previews are stale — old mux-era layout, ignored).
- [x] **Layer set complete** for fab: 2 outer + **2 inner copper (Inner1/Inner2)**,
      both masks/silks, **top paste only → all SMD top-side** (no bottom SMD — confirm
      intended), board-outline, 3 drill files, flying-probe. Board **178.05×75.69 mm**.
- [x] **Impedance stackup SET = `JLC04161H-7628`** (JLCPCB 4-layer, 1.6 mm; L1→L2
      = 0.2104 mm 7628 prepreg). **Ethernet diff pairs present at W=0.2 mm** (8 mil) →
      100 Ω differential; TXP/TXN & RXP/RXN run ~10–12 mm U19→magjack J6 on top. GOOD.
- [x] **U20 GP8403 analog + CN9** placed top; EP→GND; nets per netlist. GOOD.
- [x] **U5 SP3485EN SOIC-8** placed (RS485), pinout verified. GOOD.
- [x] **Fab limits OK for JLCPCB**: min via drill **0.305 mm** (≥0.3 std), min trace
      **0.203 mm / 8 mil** (≫3.5 mil min), 1033 vias, standard hole sizes. 2× 3.3 mm
      NPTH mounting holes (a 178 mm board often wants 4 — confirm mechanical intent).
- [ ] 🛒 **ORDER-TIME CHECKLIST (JLCPCB):** (1) **4-layer**, 1.6 mm; (2) **Impedance
      Control = YES**, stackup **JLC04161H-7628**; (3) confirm Inner1(L2) is solid GND,
      unbroken under the U19→J6 pairs; (4) pairs stay top-layer, intra-pair length
      matched (raw pad offset ~2.3–2.7 mm — fine for 100BASE-TX). Project renamed to
      **"V1.5 Final"** (matches silk); still confirm 4-layer at order.
- [x] **Final routing pass re-verified 2026-07-11** (Gerber 01:50, `.epro2` V1.5
      Final): flying-probe connectivity unchanged vs the reviewed design — all parts
      present, no stray muxes, diff pairs intact, stackup + 0.2 mm pairs retained,
      fab limits unchanged. Copper edit was routing polish only. Netlist connectivity
      unaffected (`Netlist_Schematic1_2026-07-10.net` still current for wiring).

## PCB LAYOUT — reviewed 2026-07-09 (`Gerber_PCB1_2026-07-09.zip`)
4-layer, 178.3×76.0mm. **Inner1 = solid GND plane (unbroken); Inner2 = full plane
too** (confirm its net). 1106 stitching vias + component holes. Reviewed via
rendered layers.
- [x] **Buck U2 EP → GND**: via-stitched thermal array present under U2, plus dense
      GND stitching. GOOD. (SW→L short: U2/U3 adjacent; HF input cap C3 at U2.)
- [x] Encoder receivers U7–U11 placed AT their DB9s (best differential SI). GOOD.
- [x] W5500: decoupling clustered at U19 AVDD pins, X2+load caps close, EXRES/PMODE
      in place, diff pairs routed together to J6. GOOD.
- [x] MCU: clean 45° fan-out, crystal caps (C17/C21) at U6, VCAP C22 near. GOOD.
- [ ] ~~U5 DFN EP → GND~~ — moot once U5 → SP3485EN SOIC-8 (no exposed pad).
- [ ] 🔍 Confirm **Inner2 plane net**; if it's a 2nd GND, verify 5V/3V3/24V trace
      widths on outer layers carry the current (power traces looked adequately wide).
- [ ] Local 100nF per logic IC (14×100nF on board) — verify per-IC coverage;
      TLP2309 datasheet wants a 0.1µF at each opto's Vcc(6)–GND(4).
- [ ] 🔍 Cap voltage/case: CIN C1/C2 = 1206 10µF (design specified 1210/50V);
      COUT C8/C10/C12/C13 = 22µF **0805** (design specified 1210). Confirm
      CIN ≥50V, COUT ≥16–25V; account for DC-bias derating in the smaller cases.
- [ ] 🔍 Verify diode orientation in schematic: D1 (TVS, shunt→GND), D2 (SL10100
      series→VIN, reverse-polarity), D3/D4 (1N5819WS reset steering).
- [ ] MCU crystal X1 (12pF) load caps vs its CL — verify. (W5500 X2 18pF = OK.)
- [ ] Keep buck hot loop tight; FB trace away from SW/inductor; W5500 diff pairs
      length-matched; RJ45 shield/Bob-Smith (HR911105A likely internal — verify).

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
- `board_review_todo.md` — full-board review findings (re-verified vs current export).
- `encoder_input_todo.md` — **live** phased tracker for the pending encoder restructure.
- `power_supply_todo.md` — historical working notes (power stage complete).
