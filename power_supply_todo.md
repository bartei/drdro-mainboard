# Power supply todo

> ⚠️ HISTORICAL working notes (design evolution). Many items are superseded/done.
> See **STATUS.md** for the authoritative current state and open items.
>
> **Re-verified 2026-07-04** vs `Netlist_Schematic1_2026-07-04.net` (171/133, no
> dangling nets): buck + FB divider (4.82–5.20V) + VCAP 2.2µF + RS485 GPIO-DE all
> intact. **Designator note:** the current export has buck = **U2** (LMR33630),
> inductor = **U3** (CYA0630-10µH), AMS1117 = U4, RS485 = U5 — the design docs'
> older "U1"/"U6" references map to these.

## Phase 0 — Part selection
- [x] Select buck controller (LMR33630ADDAR)
- [x] Select inductor (CYA0630-10UH, 10µH)
- [x] Verify JLCPCB stock ≥1000 units, no lifecycle warnings
- [x] Work out FB divider / inductor / cap sizing

## Phase 1 — Schematic capture
- [ ] Place LMR33630ADDAR + support components per power_supply_design.md
- [ ] Confirm footprint (HSOIC-8 PowerPAD / DDA) matches JLCPCB part
- [ ] Route thermal pad to ground plane with via array

## Phase 2 — Review (power stage)
- [x] Send netlist back for review (Netlist_Schematic1_2026-07-03.net)
- [x] Fix: U1 PGND tied to GND net (verified round 2)
- [x] Fix: R3 100k moved to VCC→PG; PG no longer floating (verified round 2)
- [ ] Optional: rename U6 (inductor) to L-prefix for BOM clarity
- [ ] Optional: recenter FB trim range (swap R30 20k→22-24k) if desired

## Phase 2b — Full-board review findings (round 2, 2026-07-04)
- [ ] 🟠 VCAP C16: change 10µF → 2.2µF X7R (STM32F411 spec)
- [ ] 🟠 Add local 100nF decoupling per logic IC (esp. 10× NC7SZ157 muxes)
- [ ] 🔍 RS485 bias R57/R63=120Ω: KEEP if staying with auto-direction (mark held by bias); only raise to ~560-680Ω if moving to GPIO/always-on DE
- [ ] Decide RS485 speed: circuit good to ~250kbps (500k marginal). For faster, GPIO-DE off USART TC (F411 has no hardware DEM) or speed up Q1 (R1→4.7-10k + 100pF speedup cap)
- [ ] 🔍 Confirm U5 SIT3088 DFN exposed pad tied to GND on layout
- [ ] 🔍 Verify crystal load caps C18/C19 (12pF) match X1 CL
- [ ] ⚪ ISO_IN1–6: rely on MCU internal pull-ups; consider 10k externals
- [ ] ⚪ ULN2003 out pull-up via LED+1.5k to 5V — verify high level for ext driver
- [ ] ⚪ U16 unused receiver channels 3&4: tie inputs to defined level
- [ ] ⚪ NRST: optional 100nF to GND
- [ ] Re-export netlist and re-review after fixes

## Phase 2c — Round 3 review (RS485 DE→MCU, 2026-07-04)
- [x] DE control moved to MCU GPIO PA11 (Q1/R1/R2 removed) — verified
- [x] RS485 bias/term redone: R28/R30=680Ω bias, R33=120Ω term — verified
- [x] RJ45 removed (intended); SWD still on header
- [x] 🔴 FB divider fixed: top 51k+33k=84k, bottom 20k+2k trim → 4.82–5.20V (verified round 4)
- [x] 🟠 UART_DE pull-down added: 10k to GND (verified round 4)
- [x] ⚪ Activity LEDs removed cleanly — no dangling nets (verified round 4)

## Phase 2d — Round 4 (2026-07-04): still-open round-2 items
- [x] 🟠 VCAP cap → 2.2µF (STM32F411 spec) — done, verified 2026-07-04
- [ ] 🟠 Verify 100nF local decoupling per logic IC (~8 caps for the digital rail)
- [ ] 🔍 U5 SIT3088 exposed pad tied to GND on layout
- [ ] 🔍 Crystal load caps (12pF) vs X1 CL
- [ ] ⚪ ISO_IN internal pull-ups in firmware; U16 unused RX inputs; NRST cap
- [ ] Reference: RS485 good to 10 Mbps (USART1/APB2, ~12.5 Mbps ceiling); SPI2 free on PB12-15

## Phase 3 — Prototype bring-up
- [ ] Verify 5V rail across 0–2A load
- [ ] Check efficiency / thermal at 2A continuous
- [ ] Confirm ripple/EMI acceptable near sensitive analog sections
