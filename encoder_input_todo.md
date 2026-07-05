# Encoder input stage — todo (LIVE tracker)

> Detail + rationale live in `encoder_input_design.md`. As-built = 3 shared quad
> receivers (A/B only); the 5-chip-per-DB9 + index-Z restructure is still owed.
> Verified against `Netlist_Schematic1_2026-07-04.net` (171 comp / 133 nets).

## Phase 0 — Design decisions
- [x] Approach: universal biased differential receiver, no mux, no mode select
- [x] Receiver part confirmed: AM26LV32E (3.3V out safe for PA0; ±14V rugged inputs)
- [x] Design of record: one quad receiver per DB9 (5 chips), A/B/Z + spare

## Phase 1 — Circuit values
- [x] Bias divider = 2.2k/2.2k (stiff vs 4–17kΩ receiver Rin); ref ~2.0–2.35V
- [x] A+ pull-up = 2.2k to 5V (fail-safe high)
- [x] No termination (would collapse the single-ended reference)
- [ ] Optional ~150Ω series protection R per DB9 line — not placed

## Phase 2 — A/B front-end (AS BUILT, verified)
- [x] NC7SZ157 muxes + TTL_SEL removed; receiver outputs direct to MCU
- [x] 3 shared quad receivers: U7=ENC1/2, U8=ENC3/4, U9=ENC5+spare
- [x] Per-channel 2.2k pull-up (A+) / 2.2k divider (A−) / no term
- [x] U9 spare channel inputs tied to a defined level
- [x] Right-angle DB9 DS1037-09FNAKT74-0CC used
- [x] Connector mapping J1→ENC1 … J5→ENC5 (each on its own encoder) verified
- [x] Outputs on correct MCU pins: PA8/9, PA5/PB3, PA6/7, PB6/7, PA0/1
- [x] No dangling nets in front-end

## Phase 3 — 5-chip-per-DB9 + index-Z restructure (OPEN — owed work)
- [ ] Split the 3 shared receivers into 5 (one AM26LV32E at each DB9)
- [ ] Wire index Z per axis. DB9 has only pin 6 free (pins 5/9 = I²C) →
      decide: single-ended Z on pin 6, OR reclaim I²C pins for differential Z
- [ ] Route the 5 Z inputs to free MCU GPIOs (PC13/14/15, PA2/3/4, PC8/9, PD2, PB4/5)
- [ ] Tie spare channels of the new receivers to a defined level
- [ ] Re-check JLCPCB stock covers 5× AM26LV32EIDR (≥1000 rule)

## Phase 4 — Review
- [ ] Re-export netlist and re-review encoder front-end after the restructure
- [ ] Decide whether I²C (SCL/SDA on DB9 pins 5/9) stays given Z pin pressure
