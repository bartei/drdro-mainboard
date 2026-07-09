# Full-board review findings — 2026-07-04

Netlist: Netlist_Schematic1_2026-07-04.net (145 comp / 120 nets at review time).
Full audit incl. MCU pin-function check vs F411 AF table, reset, boot0, all I/O.

> **Re-verified 2026-07-04** against the current export (**171 comp / 133 nets,
> zero dangling nets**): findings below still hold; deltas ticked inline.
> `STATUS.md` is the authoritative status.

## 🔁 Adversarial re-review — 2026-07-09 (`Netlist_PCB1_2026-07-09.net`, 177/133)
Deterministic parse (both `.tel` and `.net`) + full net-by-net trace + datasheet
pinout checks (STM32F411, W5500 vs WIZnet ref sch, AM26LV32E, TLP2309, SIT3088,
ULN2003, LMR33630). **No critical/board-killer defects.** Zero dangling nets,
**zero pins on >1 net (no shorts)**, all IC power/GND/EP pins resolved, all rails
sourced. Part numbers confirmed (U2 LMR33630ADDAR, U5 SIT3088ETK, U7–U11
AM26LV32EIDR, U12–17 TLP2309, U18 ULN2003ADR, U19 W5500, D1 SMDJ36CA, D2 SL10100).
New / changed items (full list + detail in `STATUS.md` OPEN):
- ✅ Encoder **5-chip-per-DB9 split now done** (U7–U11); SPI-NSS pull-up added (R72);
  W5500 AVDD-ferrite / 1V2O 10nF / TOCAP 4.7µF / EXRES 12.4k all match WIZnet ref.
- ✅ **CN1** — NON-ISSUE: it's the pluggable mating plug in the BOM, intentionally unwired.
- ✅ **Optos OK as designed** — keep board-5V sinking inputs; domain change = pin swap if ever wanted.
- ⬜ **Add ESD/surge on IN1–6**: SMAJ6.0CA (C466511) TVS→GND per line (+opt ~100Ω series R).
- ✅ **Index-Z DESCOPED** (user 2026-07-09) — front-end final, A/B only.
- ✅ **I²C pull-ups added** (R14/R74 4.7k→3V3). ESD footprints still optional.
- ✅ **U5 RS485 part change** — SIT3088ETK OOS → SP3485EN-L/TR (C8963, SOIC-8, drop-in).
- 🔧 **Replace D1 TVS** → SMCJ30A (C408374 / C135160), uni, 30V, same SMC footprint.
- 🔍 CIN 1206 / COUT 22µF-0805 case + V-rating; per-IC 0.1µF (opto Vcc). (layout)
- ✅ **Layout reviewed** (Gerber 2026-07-09): 4-layer, solid GND plane, buck EP
  stitched, receivers at DB9s, W5500 decoupling tight. See STATUS.md.

## 🔴 Critical (blocks bring-up)
- [x] MCU VDD pin 19 + VSS pin 18 — FIXED & verified: 18→GND, 19→3V3, all 5
      VDD/VSS pairs now wired.

## 🟠 Should-fix
- [x] Orphaned motor pull-ups — FIXED: reconnected to M*_OUT nets (+ CN6/CN7),
      no more dangling.
- [x] M3_DIR moved off PC13 → PC3. Also M2_STEP/DIR, M3_STEP → PC0/PC1/PC2.
      All normal GPIOs; vacated pins freed; no double-assignment. Verified all
      7 channels MCU→ULN→connector.
- [ ] 🔵 Connector grouping heads-up: motor 2 split across CN6 (M2_STEP) and
      CN7 (M2_DIR). Electrically fine; regroup per-motor if desired. CN6/CN7
      are signal-only (no GND/5V) → assume opto-isolated drivers, COM→driver +V.
- [ ] I2C: add SDA/SCL pull-ups + ESD footprints (expansion bus, still missing).
- [ ] Encoder front-end: only 3 shared quad receivers (A/B, no Z) — the agreed
      5-chip-per-DB9 + index-Z restructure is still owed. See encoder_input_todo.md.

## ✅ Verified correct
- [x] All 5 encoders on TIM1-5 CH1/CH2 (hardware quadrature) — pin choice good
- [x] USART1 / I2C1 / SPI2 / SWD / crystal on valid AF pins
- [x] Reset circuit (pull-up + steering diodes + button)
- [x] BOOT0 circuit (pull-down + button)
- [x] Encoder receivers direct to MCU; no 5V on non-tolerant pins
- [x] Decoupling improved (11 caps on 3V3)

## 🔵 FYI / firmware
- STEP pins not on free timers (TIM1-5 = encoders) → GPIO/DMA step gen only
- ENC5 firmware: use TIM5 (AF2) not TIM2
- JTAG pins repurposed — SWD-only debug (fine)

## W5500 Ethernet (added 2026-07-04) — mostly correct
- [x] Crystal load caps → 18pF (C28/C31). CORRECT for the X322525MOB4SI which
      is a 12pF-CL crystal (2×(12−~3pF stray)=18pF → ~12pF effective CL). Note:
      earlier "match to 18pF" was right by luck; crystal is 12pF-CL not 18pF.
- [x] Crystal case pins (X3 pins 2,4) grounded.
- [ ] SPI NSS (PB12→W5500 SCSn): wiring correct. FIRMWARE: drive PB12 as GPIO
      software CS (not HW-NSS) so SCSn frames each W5500 SPI transaction (VDM).
- [ ] 🟠 Optional: add ~10k pull-up on SCSn→3V3 (defined deselect at boot).
- [ ] 🔵 Optional: add 1MΩ feedback (XI-XO) + 0Ω series on XO per ref (Fig 3).
- [x] SPI mapping correct (MOSI-MOSI/MISO-MISO, SCSn→PB12, no swap); INTn/RSTn
      →PC6/PC7 with pull-ups — verified vs datasheet Fig 4
- [x] EXRES1 12.4k 1%→AGND, 1V2O 10nF, TOCAP 4.7µF — all per WIZnet ref
- [x] AVDD ferrite+decoupling, center-tap 3V3 via ferrite+0.1µF, PMODE=111
- [x] TX/RX pairs + MagJack + LEDs correct; NC pins (7,12,13,18,24,26,46,47) ok

## ⚪ Minor / carryover
- [ ] RESET optional 100nF cap to GND
- [x] SPI (PB12-15) now wired to W5500 (SCSn/SCLK/MISO/MOSI + INTn/RSTn) — verified
- [ ] Carryover verify: U5 SIT3088 EP→GND, crystal load caps vs CL, ISO_IN
      firmware pull-ups
