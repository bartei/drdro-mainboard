# Analog outputs (2× 0–10 V VFD ref) — todo (LIVE tracker)

> Detail + rationale in `analog_output_design.md`. Scope: 2× 0–10 V via one GP8403
> dual I²C DAC on the shared I²C1 expansion bus (PB8/PB9), non-isolated. 4–20 mA
> dropped (2026-07-10). **In the netlist + reviewed 2026-07-10** (`Netlist_Schematic1_2026-07-10.net`).

## Phase 0 — Decisions
- [x] 0–10 V only; 4–20 mA dropped (user, 2026-07-10)
- [x] Non-isolated (board/VFD shared cabinet ground)
- [x] Part: GP8403-TC50-EW dual 12-bit I²C→0–10 V (C3152007)
- [x] Bus: **shared I²C1 expansion bus** (PB8/PB9), addr 0x58 (not the bit-bang bus)

## Phase 1 — Schematic (DONE, verified in netlist)
- [x] U20 GP8403 placed; VCC → VIN (24 V); GND + EP → GND
- [x] V5V → C45 (1 µF) to GND
- [x] SCLK/SDA → SCL/SDA (I²C1); pull-ups R74/R14 4.7 k reused; A0/A1/A2 → GND (0x58)
- [x] VOUT0→AO0, VOUT1→AO1; each 1 µF (C42/C43) + SMAJ12A TVS (D5/D6, C113957) to GND
- [x] 47 Ω series R per output (R75/R76)
- [x] Output terminal CN9 (WJ15EDGRC-3.81-4P): AO0 / AO1 / GND / GND
- [ ] Optional: local 0.1 µF + 10 Ω/ferrite + 10 µF RC on VCC (24 V-bus noise)
- [ ] Reserve I²C addr 0x58 — no future DB9-bus expansion device may reuse it

## Phase 2 — Layout
- [ ] Place U20 away from buck/inductor + motor-driver noise
- [ ] Short VOUT traces; TVS + cap at the connector end
- [ ] AO COM to solid GND plane; keep any VCC filter local

## Phase 3 — Firmware
- [ ] I²C1 driver reaches U20 @ 0x58 (share bus w/ any DB9 expansion devices)
- [ ] Set 0–10 V mode (reg 0x01 = 0x11) at boot; optional NVM persist
- [ ] RPM→12-bit scaling per VFD; write VOUT0 (0x02) / VOUT1 (0x04)

## Phase 4 — Review
- [x] Netlist review 2026-07-10: AO block wiring correct, no dangling nets, EP→GND,
      addr 0x58, VOUT protection present, caps 1 µF per official datasheet
- [ ] Re-confirm JLCPCB stock/part-class for C3152007 before order