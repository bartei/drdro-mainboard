# Encoder input stage — universal single-ended + differential

## Goal
Accept BOTH single-ended TTL and differential RS-422 encoder/scale inputs on
the same DB9, with **no mux and no mode selection** — "just works" like the
cheap Chinese DROs. Approved 2026-07-04.

## AS-BUILT vs TARGET (status 2026-07-09)
**As built** (netlist `Netlist_PCB1_2026-07-09.net`, verified): the mux is gone,
the biased-receiver technique is in, and the **per-DB9 split is DONE — 5 quad
receivers, one AM26LV32EIDR at each connector, A/B only**:
- U7 = J1/ENC1, U8 = J2/ENC2, U9 = J3/ENC3, U10 = J4/ENC4, U11 = J5/ENC5
- Per line: 2.2k pull-up on A+/B+, 2.2k/2.2k divider on A−/B− (~2.5V ref), no term
- Receivers enabled (pin4/G = 3V3); each chip's 2 spare channels have inputs tied
  to a defined level, outputs left open
- Receiver outputs direct to MCU (PA8/9, PA5/PB3, PA6/7, PB6/7, PA0/1 → TIM1–5).
  **No index-Z wired yet.**

**Remaining vs design of record:** only **index-Z** is still to add (A/B/**Z** +
spare per the "Modularity" section below). Reuse a spare receiver channel per chip
for Z; DB9 pin 6 is the only free connector pin (see Z-routing constraint).

**Z-routing constraint:** each DB9 (DS1037-09) has only **pin 6** free — pins
5/9 carry the I²C SCL/SDA expansion bus. So a *differential* Z (Z+/Z−) has no
room unless I²C is dropped/moved; a **single-ended Z on pin 6** is the drop-in
option. MCU side is not the limit: 11 free GPIOs (PC13/14/15, PA2/3/4, PC8/9,
PD2, PB4/5) — ample for 5 Z inputs. Tracker: `encoder_input_todo.md`.

## Decision
Replace the current dual-path front-end (AM26LV32E differential receiver +
NC7SZ157 mux + TTL_SEL) with a **single universal biased differential
receiver** per channel, no termination, no mux.

### The technique (industry-standard; see references)
A differential line receiver reads single-ended by biasing the complementary
(−) input to a fixed reference at **half the encoder supply (2.5 V for 5 V
scales)**, per Emerson/Control Techniques CTTN #155 ("bias the complementary
channels... to ½ of the encoder supply") and confirmed by TouchDRO ("RS-422
line receivers also handle single-ended quadrature inputs cleanly").

Per differential pair (e.g. A+/A−):
- **A+ → pull-up to 5 V** (fail-safe high when idle; also serves open-collector
  single-ended scales)
- **A− → divider to ~2.5 V reference** (½ of the 5 V scale supply)
- **NO termination** (the old 120 Ω is what collapsed the reference and forced
  the mux; low-Z termination breaks single-ended — CTTN #155 disables term in
  this mode)
- optional **~150 Ω series** resistor from the DB9 pin for ESD/fault protection

Behaviour, all with one input, no switching:
| Scale | A+ | A− | Receiver sees | Output |
|---|---|---|---|---|
| Single-ended | 0–5 V signal | 2.5 V ref | signal vs 2.5 V | clean ✓ |
| Differential | driven | driven (low-Z driver **overrides** the kΩ bias) | true differential | clean, full CMRR ✓ |
| Disconnected | pulled to 5 V | 2.5 V | +2.5 V | defined HIGH → no phantom counts ✓ |

The differential driver "takes over" the − line because the bias is high-Z
(kΩ) and the line driver is low-Z. That's the whole trick.

### Receiver part: AM26LV32E (keep) — confirmed right
Quad differential line receiver, already in the design. Datasheet SLLS849E:
- A/B input abs-max **±14 V** → rugged for external DB9 (miswire/ESD/any scale)
- VIH to **5.5 V**, common-mode **−7…+7 V** → handles 2.5 V bias + 5 V swing
- **3.3 V supply, 3.3 V logic output** → safe for ALL MCU pins incl. **PA0
  (ENC5A), which is 3.3 V-only/not 5 V-tolerant**. A 5 V receiver (AM26C32/
  26LS32) would over-drive PA0 → rejected for that reason.
- Failsafe ("E" variant).

### Modularity: one quad receiver per DB9 connector
Go **one AM26LV32E per DB9** (5 chips vs current 3):
- Self-contained per-axis block → copy-paste schematic + PCB, better layout.
- Receive differential AT the connector → best signal integrity.
- Per-axis fault isolation.
- Quad = A + B + **Z (index)** + 1 spare → future-proofs the reference mark
  (DRO homing) with no redesign. Currently only A/B wired.
- Tie spare channel inputs to a defined level.

### Open detail — bias divider vs receiver input R
AM26LV32E input resistance is 4–17 kΩ, which loads the 2.5 V divider (a naive
10 k/10 k sags to ~1.9 V). Fix when specing values: use a stiffer divider
(~2.2 k/2.2 k) OR one shared buffered 2.5 V reference fed through per-channel
series R. Reference anywhere ~2.0–2.5 V is fine (0–5 V swing gives margin).

## Parts removed vs current design
- NC7SZ157 muxes (U22–U31, all 10) — deleted
- 1 kΩ I0 series resistors (R78 ×10) — deleted
- 120 Ω terminations (R47 ×10) — deleted
- TTL_SEL GPIO (PA4) — freed
- Route AM26LV32E outputs directly to the MCU encoder pins.

## Caveat (inherent to single-ended, per CTTN #155)
Single-ended has no common-mode rejection on that channel → more noise-
sensitive than differential. Good grounding/shielding matters. A differential
scale on the same input still gets full noise rejection. This is the exact
trade-off cheap DROs accept.

## References
- Emerson/Control Techniques CTTN #155 "Accommodating Single Ended Encoders"
- TouchDRO glass-scale resources (touchdro.com)
- TI AM26LV32E datasheet SLLS849E
