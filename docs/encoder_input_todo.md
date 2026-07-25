# Encoder input stage — todo (LIVE tracker)

> Detail + rationale live in `encoder_input_design.md`. As-built (verified against
> `Netlist_PCB1_2026-07-09.net`, 177 comp / 133 nets) = **5 quad receivers, one
> AM26LV32E per DB9, A/B only**. The 5-chip split is DONE; **index-Z is DESCOPED**
> (user, 2026-07-09) → front-end is FINAL.

## Phase 0 — Design decisions
- [x] Approach: universal biased differential receiver, no mux, no mode select
- [x] Receiver part confirmed: AM26LV32E (3.3V out safe for PA0; ±14V rugged inputs)
- [x] Design of record: one quad receiver per DB9 (5 chips), A/B/Z + spare

## Phase 1 — Circuit values
- [x] Bias divider = 2.2k/2.2k (stiff vs 4–17kΩ receiver Rin); ref ~2.0–2.35V
- [x] A+ pull-up = 2.2k to 5V (fail-safe high)
- [x] No termination (would collapse the single-ended reference)
- [ ] Optional ~150Ω series protection R per DB9 line — not placed

## Phase 2 — A/B front-end + 5-chip split (AS BUILT, verified 2026-07-09)
- [x] NC7SZ157 muxes + TTL_SEL removed; receiver outputs direct to MCU
- [x] **5 quad receivers, one AM26LV32EIDR per DB9: U7=J1, U8=J2, U9=J3, U10=J4, U11=J5**
- [x] Per-line 2.2k pull-up (A+/B+) / 2.2k+2.2k divider (A−/B− → ~2.5V) / no term
- [x] Receivers enabled: pin4(G)=3V3; spare ch3/4 inputs tied to defined levels,
      outputs (pin11/13) left open
- [x] Right-angle DB9 DS1037-09FNAKT74-0CC used (J1–J5)
- [x] Connector mapping J1→ENC1 … J5→ENC5 (each its own receiver+axis) verified
- [x] Outputs on correct MCU pins: PA8/9, PA5/PB3, PA6/7, PB6/7, PA0/1 (TIM1–5)
- [x] No dangling nets / no shorts in front-end
- [ ] Note: receiver output is inverted vs input (A+ lands on the B-input pin);
      consistent across all 5 → handle as a firmware count-direction flip only

## Phase 3 — index-Z — DESCOPED (user decision 2026-07-09)
- [x] **Z inputs will never be connected.** No work owed. DB9 pin 6 stays free;
      each chip's 2 spare receiver channels stay tied off. Encoder front-end is
      FINAL: 5× AM26LV32E, A/B only, one per DB9.
- [x] JLCPCB stock: 5× AM26LV32EIDR already placed (re-confirm ≥1000 before order)

## Phase 4 — I²C expansion bus
- [x] Pull-ups added: R14 (SDA) / R74 (SCL) = 4.7k to 3V3. DONE 2026-07-09.
- [ ] Optional: ESD footprints on SCL/SDA where they leave via the DB9s.

## Phase 5 — Review
- [x] Re-exported netlist + Gerber re-reviewed 2026-07-09 (front-end verified;
      receivers placed at their DB9s, good SI). Front-end FINAL.
