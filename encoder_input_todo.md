# Encoder input stage todo

> ⚠️ HISTORICAL working notes (design evolution). Implementation is done & verified.
> See **STATUS.md** for the authoritative current state and open items.

## Phase 0 — Design decisions
- [x] Approach: universal biased differential receiver, no mux, no mode select
- [x] Receiver part confirmed: AM26LV32E (3.3V out safe for PA0; ±14V rugged inputs)
- [x] Modularity: one quad receiver per DB9 connector (A/B/Z + spare)

## Phase 1 — Circuit values
- [ ] Compute bias divider stiff enough vs 4–17kΩ receiver input R (~2.2k/2.2k) OR shared buffered 2.5V ref + per-channel series R
- [ ] A+ pull-up to 5V value (~10k)
- [ ] Optional series protection R (~150Ω) per DB9 line
- [ ] Confirm 2.5V ref = ½ of 5V scale supply

## Phase 2 — Schematic changes
- [ ] Delete NC7SZ157 muxes (U22–U31), R78 (1k I0), R47 (120Ω term)
- [ ] Re-org to one AM26LV32E per DB9 (5 chips); wire A/B (+Z optional)
- [ ] Route receiver outputs directly to MCU encoder pins
- [ ] Tie spare receiver channel inputs to defined level
- [ ] Free/reuse TTL_SEL GPIO (PA4)
- [ ] Verify JLCPCB stock covers 5× AM26LV32E per board (≥1000 rule)

## Phase 2 — implementation review (2026-07-04)
- [x] Muxes removed; receiver outputs direct to MCU — verified
- [x] Per-channel A+ pull-up / A- 2.5V divider / no termination — verified
- [x] Spare receiver channels (U12 ch3/4) inputs tied to defined level — verified
- [x] Right-angle connector DS1037-09FNAKT74-0CC swapped in — verified
- [x] 🔴 CONNECTOR BUG FIXED: J1→ENC1 … J5→ENC5, each on its own encoder (verified)
- [x] 🟠 Bias changed 10k → 2.2k (pull-up + divider) — ref now ~2.0–2.35V, fail-safe margin +1.3…2.1V (verified)
- [x] ⚪ TTL_SEL orphan removed (no longer dangling)
- [ ] Confirm I2C (SCL/SDA) on DB9 pins 5/9 is intentional
- [ ] SPI bus (PB12-15) WIP per user — not connected yet

## Phase 3 — Review
- [ ] Re-export netlist and review encoder front-end after fixes
