# drDRO mainboard — STM32H723 variant (design of record)

> **Status:** initial design, 2026-07-15. This is a **new variant**, not an edit of
> V1.5. The V1.5 board (STM32F411 + W5500 + GP8403 I²C DAC) stays as-is; this folder
> holds the higher-performance H723 line. Parent-board context and the review history
> live in the repo root (`STATUS.md`, `mcu_pinmap.md`, `power_supply_design.md`,
> `encoder_input_design.md`, `analog_output_design.md`). Reuse everything that still
> applies (encoder front-end, buck stage, RS-485 DE topology, protection philosophy).
>
> Part facts + JLCPCB/LCSC stock below checked **2026-07-15** — re-verify at order
> time, stock changes daily. Proposed MCU pin assignments are **preliminary** until
> the dedicated AF audit (see Phase 2 / future `H723_pinmap.md`).

---

## 1. Goal & scope

Build a faster, higher-I/O sibling of the V1.5 DRO mainboard on the **STM32H723**,
keeping a **similar board outline** but roughly doubling usable I/O by moving to
denser terminal blocks and folding three external functions into the MCU:

- **Native Ethernet** — drop the external **W5500** (MAC+PHY+TCP-in-silicon). The
  H723 has an on-chip **Ethernet MAC**; it needs only a small external **RMII PHY**
  (LAN8742A, QFN-24) instead of the large W5500 + its magnetics-side SPI plumbing.
- **Native USB** — the H723 has an **on-chip USB full-speed PHY**, so a host PC can
  talk to the board over USB with **no external USB chip** (just D+/D− + ESD).
- **Direct analog** — analog **outputs** from the MCU's **2× internal DACs** (+ a gain
  stage) and analog **inputs** into the MCU's **16-bit ADCs**, replacing the external
  **GP8403 I²C DAC** and any I²C analog front-end.

**Hard requirements (user):**
- ≥ **16 digital outputs** (up from ~8).
- Analog in + analog out wired **directly to the MCU** (no I²C interface chip).
- Native Ethernet (no W5500) and native USB.
- Denser terminal blocks so I/O density rises on a similar outline.

**Stretch / optional:** 2nd **RS-485** port for an expansion bus; **FDCAN** expansion
bus; more isolated inputs; external voltage reference for ADC accuracy.

---

## 2. Why STM32H723 (vs the F411 on V1.5)

| Axis | F411 (V1.5) | H723 (this variant) |
|---|---|---|
| Core | Cortex-M4F @ 100 MHz | **Cortex-M7 @ 550 MHz** (L1 cache, DP-FPU) |
| Flash / RAM | 512 KB / 128 KB | **1 MB / 564 KB** |
| Ethernet | external W5500 (SPI) | **integrated MAC** + tiny RMII PHY |
| USB | none used | **on-chip FS PHY** (device: CDC/DFU direct) |
| DAC | none (external GP8403 I²C) | **2× 12-bit DAC** on-chip |
| ADC | 1× 12-bit | **3× ADC, 16-bit** (ADC1/2), fast |
| Op-amps | none | **2× internal op-amps** (analog-in buffering) |
| Timers | 8 (all 5 used by encoders → no HW step-gen) | **~16 timers** → HW quadrature **and** HW step-gen simultaneously |
| Comms | 3× U(S)ART, 5 SPI/I²C total | 8× U(S)ART, 6 SPI, 4 I²C, **3× FDCAN** |

Two structural wins beyond raw speed:
1. **Timers are no longer the bottleneck.** On F411 all 5 timers were consumed by
   encoders, forcing GPIO/DMA step generation. The H723 has enough timers to run **5
   hardware quadrature encoders AND hardware-PWM step generation** at once.
2. **Fewer external ICs.** W5500 → (MAC + small PHY); GP8403 DAC → (internal DAC +
   op-amp); no external USB PHY. Net silicon count and board area drop even as I/O
   grows.

---

## 3. System block diagram (text)

```
        24 V in ── protection (Schottky + TVS + PPTC) ──┬─ LMR33630 buck ─ 5V ─ AMS1117/buck ─ 3V3 ─ MCU/PHY/logic
                                                         ├─ (opt) 12 V rail ─ analog-out op-amps
                                                         └─ TBD62083 output-bank COM (loads sink to field 24 V)
                                            ┌───────────────────────────────────────────────┐
   5× DB9 encoders ─ 5× AM26LV32E ──────────┤                                               │
   (diff RS-422 / SE TTL, per V1.5)         │                                               │── RJ45 ─ LAN8742A (RMII) ─ ETH MAC
                                            │                STM32H723ZGT6                   │── USB-C/µB ─ USB FS (on-chip PHY)
   8–16 isolated inputs ─ 2-4× TLP291-4 ─────┤                  (LQFP-144)                    │── 2× RS-485 (SP3485/THVD)
                                            │                                               │── (opt) 1× FDCAN
   4–8 analog inputs ─ divider+clamp+RC ────┤ ADC                                     DAC ──┤── 2× 0–10 V analog out (op-amp gain)
                                            │                                               │
   16 digital outputs ◄─ 2× TBD62083 ◄──────┤ GPIO (BSRR word)              I²C/SWD/BOOT ───┤
                                            └───────────────────────────────────────────────┘
```

---

## 4. MCU — STM32H723ZGT6 (LQFP-144)

**Part of record:** STM32H723ZGT6, **LCSC/JLCPCB C730146**, LQFP-144 (20×20 mm,
0.5 mm pitch), 1 MB flash / 564 KB RAM, ~$6.6 @1 (2026-07-15). LQFP-144 chosen over
LQFP-100 (`STM32H723VGT6`) to leave generous pin headroom for RMII + 16 outputs +
analog + 2× RS-485 without fighting for pins; still hand/JLCPCB-assemblable (no BGA).

### 4.1 Power scheme (H7 is more involved than F4)
The H7 has a dual internal-regulator scheme. **Decision: use LDO mode** (SMPS
disabled) for the first spin — simplest, lowest-risk, well-documented. Implications:
- **VDD = 3.3 V** across all VDD pins, each with local 100 nF + bulk.
- **VCAP pins** → the internal-LDO output caps (2× 2.2 µF per the H7 power-scheme
  table). Do **not** load externally.
- **VDDA / VREF+** → clean analog 3.3 V (ferrite + local bulk), or an **external
  VREF** (see §6.4). VREF+ decoupling: 1 µF + 100 nF minimum.
- **VBAT** → 3V3 (no coin cell; RTC not battery-backed unless a cell is added).
- **VDD33USB** → 3.3 V for the USB FS transceiver (its own decoupling).
- **Follow the exact power-supply scheme + decoupling map from the H723 datasheet /
  AN5342 in the schematic phase** — the H7 pin-by-pin supply map is unforgiving and
  must be copied exactly. (SMPS mode is a later efficiency option: it needs an
  external inductor on the SMPS pins and reconfigures VCAP — deferred.)

### 4.2 Clocking
- **HSE = 25 MHz crystal** on PH0/PH1. 25 MHz serves the PLLs (→ up to 550 MHz core,
  and the exact 48 MHz for USB via a PLL, and the Ethernet 50 MHz domain) and matches
  the LAN8742A's 25 MHz crystal so the Ethernet clock tree is coherent.
- **USB 48 MHz**: from a PLL off HSE (deterministic). HSI48 + CRS is a fallback but
  HSE-derived is preferred once the crystal is present.
- **Ethernet RMII 50 MHz REF_CLK**: sourced by the **PHY** (LAN8742A internal PLL
  off its 25 MHz crystal) and fed into the MCU RMII_REF_CLK — the standard ST-Nucleo
  topology (see §5.1). Avoids the MCU having to output a clean 50 MHz on MCO.

---

## 5. Connectivity

### 5.1 Ethernet — RMII + LAN8742A (replaces W5500)
- **PHY of record:** **LAN8742A-CZ-TR**, Microchip, **C621424**, QFN-24-EP (4×4 mm),
  ~$0.97 (2026-07-15). 10/100BASE-TX, RMII, the exact PHY on the ST Nucleo-H7 boards
  → proven reference schematic, well-supported in CubeMX/LwIP.
- **Interface: RMII** (9 signals: REF_CLK, MDC, MDIO, CRS_DV, RXD0, RXD1, TXD0, TXD1,
  TX_EN) — half the pins of MII, plenty of bandwidth for 100BASE-TX. Plus PHY **nRST**
  (GPIO) and optional **nINT** (GPIO).
- **Clock:** 25 MHz crystal at the PHY; PHY PLL outputs 50 MHz on its REF_CLKO/nINTSEL
  strap → 100 Ω series → MCU RMII_REF_CLK. Keep this trace short/matched.
- **Magnetics + RJ45:** use an **integrated-magnetics RJ45** (e.g. HanRun HR911105A
  class, as on V1.5's W5500 side) — carry the same MagJack forward. TX±/RX± are the
  only 100 Ω differential pairs on the board → route as pairs over solid GND, same
  discipline as V1.5's W5500 diff pairs (the 4-layer JLC04161H-7628 stackup already
  supports this).
- **Straps:** set PHY address + REF_CLK-out mode + auto-neg via resistor straps per
  the LAN8742A schematic checklist. Decouple every VDD pin; the internal 1.2 V
  regulator cap per datasheet.

### 5.2 USB — full-speed device, on-chip PHY (new capability)
- H723 **USB OTG** with **on-chip FS PHY** → only external parts are the **D+/D−**
  lines, a **USB ESD array** (e.g. USBLC6-2SC6, already the go-to), and the connector.
- **Connector:** USB-C (receptacle, CC 5.1 kΩ pulldowns for UFP) recommended over
  micro-B for durability; either is fine. VBUS → **VBUS-sense** GPIO through a divider
  (self-powered board: detect host presence, do **not** back-power from VBUS).
- **Firmware role:** USB-CDC (virtual COM) for config/console and/or DFU for firmware
  update — a big usability win over SWD-only. (True 480 Mbps HS is **out of scope** —
  it needs an external ULPI PHY; FS/12 Mbps is ample for a control board.)

### 5.3 RS-485 — 2 ports (1 required carry-over, 1 expansion)
- **Port A (host/DRO):** carry V1.5's proven topology forward — **SP3485EN-L/TR**
  (C8963, SOIC-8, 3.3 V, 10 Mbps), **GPIO-DE** (assert before TX, deassert on USART
  TC), 680 Ω/680 Ω fail-safe bias, 120 Ω termination. On a spare H723 USART.
- **Port B (expansion, optional):** 2nd transceiver on a 2nd USART for daisy-chaining
  expansion I/O modules. Same part/topology. Termination populated only if it's a bus
  end. Consider **THVD1450** (higher ESD) if field robustness matters more than the
  10 Mbps ceiling.
- H723 has 8 U(S)ARTs, so 2× RS-485 + USB-CDC + a debug UART all coexist easily.

### 5.4 FDCAN — optional expansion bus
- The H723 has **3× FDCAN**. One FDCAN + a **transceiver** (e.g. TCAN332/SN65HVD230,
  3.3 V) gives a rugged multi-drop expansion bus (better noise immunity + longer runs
  than RS-485 for distributed I/O). **Optional** — add the footprint + transceiver;
  populate if the expansion story wants CAN. 120 Ω term at bus ends.

---

## 6. I/O expansion

### 6.1 Digital outputs — 16× (requirement) via 2× TBD62083
- **Driver of record:** **2× TBD62083APG**, Toshiba, **C114143**, 8-ch **DMOS** sink
  array. Chosen over ULN2803: **VDS 0.4–0.65 V @ 200 mA** vs the Darlington's
  1.0–1.3 V → far less heat and higher usable current; **50 V / 500 mA per channel**;
  **VIN(ON) 2.5 V** so **3.3 V logic drives it directly** (no level shift). Built-in
  free-wheel clamp diodes (COM pin → field 24 V) for inductive loads (relays, valves).
- **Topology:** open-drain **low-side sink** outputs to terminal blocks, exactly like
  V1.5's ULN2003 motor drive, but 16 channels. Each output: MCU GPIO → TBD62083 in →
  channel out → terminal → external load → field +24 V; COM tied to field +24 V for
  the clamp diodes.
- **GPIO mapping — one 16-bit port word.** Drive all 16 from a single contiguous
  GPIO port (e.g. PORTE[0:15] or PORTD[0:15], both fully bonded on LQFP-144) so the
  firmware can update all 16 atomically via **BSRR / the ODR word** in one write.
- **Motor STEP/DIR/ENA** live **inside** this output bank: assign the STEP pins to
  **timer-capable GPIOs** so the H723 does **hardware-PWM step generation** (the F411
  couldn't — timers were full). DIR/ENA are plain outputs. So "3 motors" is a
  firmware assignment of ~7 of the 16 outputs, leaving ≥9 general outputs — or drop
  motors and have 16 general outputs. Flexible.
- **Optional isolation:** these are non-isolated open-drain (same as V1.5). Field ESD
  hardening: TVS to field-GND at the terminal on exposed channels. Full opto-isolated
  outputs are a later variant (16× optos = large area) — **not** in this spin.

### 6.2 Digital inputs — opto-isolated (compact via quad arrays), expand 6 → 8/16
**Decision (2026-07-16): keep galvanic isolation AND the per-channel indicator LED —
just shrink the package count.** V1.5's one-opto-per-channel (TLP2309, SOP-6) is the
area sink; swapping to **quad opto arrays** folds 4 channels into one SOP-16, so 16
inputs go from **16 packages → 4**, while isolation and the per-input LED are retained.

- **Part of record: TLP291-4** (Toshiba, **LCSC/JLCPCB C60900**, SOP-16-4.4 mm, 4×
  transistor-output optocoupler, 2.5 kV iso, CTR ≤400). JLCPCB-assembly part
  (re-verify stock/class at order — changes daily).
  - **Higher-CTR alt: TLP293-4** (**C113626**, CTR ≤600, 3.75 kV iso) — preferred if
    the series indicator LED (below) squeezes the drive-current/headroom budget, since
    higher CTR switches the output reliably at lower LED current. Small price premium.
- **Per channel (field side):** terminal (+) → **series R** → **indicator LED** (in
  series) → opto internal LED → **field COM (−)**. One resistor sets the current for
  **both** the indicator and the opto LED, so the "input active" light needs **no
  board-side power** and can't disagree with the opto state. Isolation preserved: field
  COM may be board GND *or* a separate isolated field common (the option V1.5 kept open).
- **Board side:** opto **open-collector output** → MCU GPIO with **internal pull-up** +
  small RC filter; software debounce. (Same firmware model as V1.5.)
- **Series-R sizing = field "on" voltage** (LED current target ~3–5 mA):
  - ≤10 V field → ~1–1.5 kΩ; 24 V field → ~4.2 kΩ.
  - **Wide 5–24 V range on one input:** a fixed R can't hold current across that span —
    use a **constant-current diode (CRD)** (or a 2-transistor CC sink) per channel so
    LED current stays ~constant from ~5 V to ~30 V. Adds one small part/channel; use
    only on inputs that must accept a wide range. (TLP293-4's higher CTR widens the
    usable fixed-R window before a CRD is needed.)
- **Protection:** series R already limits fault current; add an optional field-line TVS
  for surge and an optional series reverse-polarity diode if inputs may be miswired.
- **Passives as networks/arrays** (series-R networks) to further trim the bank.

**Net:** optos 4:1 vs V1.5 (16 → 4 packages), **isolation kept, per-input LED kept**,
fully JLCPCB-assemblable. Larger than the non-isolated passive option but that trade
was rejected — isolation + the indicator LEDs are wanted.

### 6.3 Analog inputs — direct to ADC (new)
- **4 analog inputs** on the first spin (footprints/terminals for up to 8), each
  **0–10 V** field range scaled to the ADC. Per channel:
  - **Divider** 0–10 V → 0–~3.0 V (e.g. 22 k / 10 k → 0–3.1 V, leaves ADC headroom).
  - **RC anti-alias / noise filter** (series R + cap) at the ADC pin.
  - **Clamp**: Schottky/TVS to the analog rail + GND, or a low-cap TVS, so field
    over-voltage can't punch the pin. Divider Thévenin (~6.9 k) is a bit high for fast
    16-bit sampling → either **buffer through an internal op-amp** (H723 has 2) or use
    a stiffer divider + long ADC sample time. **Prefer the internal op-amp buffer** —
    zero extra BOM, gives low source-Z into the ADC.
- Route analog inputs to **ADC1/ADC2** channels (16-bit); keep them clustered away
  from the buck/PHY/switching outputs.

### 6.4 Analog outputs — 2× 0–10 V, internal DAC + gain (replaces GP8403)
- **Source:** the H723's **DAC1** two channels (DAC1_OUT1 = PA4, DAC1_OUT2 = PA5),
  0–~3.3 V. No external I²C DAC.
- **Gain to 0–10 V:** non-inverting op-amp, **G ≈ 3.03** (10 V / 3.3 V). Two options —
  decide in schematic:
  - **(A) Op-amp on the 24 V rail** (e.g. LM2904/LM358-class, 36 V abs-max, cheap,
    JLCPCB-stocked). Output never approaches the rail so RRO isn't required; add a
    **pulldown** on the output so it reaches ~0 V cleanly (single-supply zero-crossing).
    No new rail. **Recommended** for simplicity.
  - **(B) Dedicated 12 V rail** + RRIO op-amp — cleaner 0 V/full-scale behaviour but
    adds a regulator. Reserve if precision at the extremes matters.
- **Per output** (carry V1.5's protection recipe): 1 µF to GND + **SMAJ12A** TVS
  (C113957) + **47 Ω series** to the terminal. VFD analog-in is high-Z so series-R
  error is negligible. Non-isolated (board & VFD share cabinet 0 V), same as V1.5.
- **Internal DAC caveat:** the DAC output buffer doesn't reach rail-to-rail near 0/3.3 V
  — keep the used code range inside the buffer's linear window (or enable the DAC in
  buffered mode with a small offset), then let the op-amp gain map it to 0–10 V. Note
  for firmware calibration.
- **Resolution:** 12-bit over 0–10 V = 2.44 mV/LSB — finer than any VFD speed ref
  needs (same as the GP8403 it replaces).

---

## 7. Terminal-block strategy (density)

Goal: more I/O on a similar outline by shrinking the connectors. The binding
constraint is **board-edge length, not MCU pins** (§9 shows ~40 pins spare), so the
connectors — not the silicon — cap how much I/O fits. V1.5 uses **WJ15EDG / 15EDGRC
3.81 mm** pluggable blocks.

**Load envelope (user, 2026-07-15):** signal currents are **tens of mA at most**,
voltages **≤10 V**, exceptionally **24 V** when driving industrial devices. That
leaves any block's current/voltage rating almost entirely unused — so the choice is
driven purely by **density + wiring ergonomics**, not by current.

- **Decision — part of record: DORABO DB141V-2.54 spring-clamp family** (user pick,
  2026-07-15). **2.54 mm (0.1″)**, **screwless spring-clamp** (tool-free, color-coded
  lever), **vertical wire entry**, fixed board-mount, **6 A / 160 V, 0.5 mm² /
  18–26 AWG**, JLCPCB-stocked in **2P–30P** (C2898744 = 2P … **C2898750 = 8P** …
  C2898753 = 12P) so each connector is sized to its function. Two solder posts for
  mechanical strain relief. Rationale:
  - **Density:** 3.81 → 2.54 mm is a **~33 % linear pitch reduction** (vs only ~8 %
    for 3.81 → 3.5 mm) — the real win for packing the **16-output bank** + inputs +
    analog along the same board edge. Vertical entry also allows **stacking rows** for
    a further density gain. This is what makes "more I/O, similar outline" work.
  - **Electrically trivial for these loads:** 6 A / 160 V is orders of magnitude over
    tens-of-mA / ≤24 V; 2.54 mm creepage is comfortably enough for 24 V in a dirty
    cabinet.
  - **Spring/push-in suits a moving machine:** tool-free, and the spring is **vibration-
    resistant** (a genuine plus over screw clamps on a motion-control board).
- **Fixed, not pluggable — trade accepted:** DB141V wires straight to the board, so we
  **lose V1.5's unplug-the-connector-to-swap-a-board** serviceability. Accepted for
  density/cost/tool-free wiring. (If board-swap serviceability later proves important,
  a pluggable 2.54 mm family can be substituted at some density/selection cost.)
- **Keep the 24 V power INPUT on a robust larger pitch** (3.81 mm / 5.08 mm screw).
  It carries the whole board current (~1–2 A) and wants a **thicker supply wire** than
  a 2.54 mm / 18–26 AWG cage accepts — the one place density doesn't matter.
- **Shared-COM strips** cut pole count: a 16-output bank needs 16 signal poles + a few
  shared field-24 V/COM poles rather than 32 — a density win on top of the pitch.
- **Confirm at layout:** **vertical** wire entry (cables exit perpendicular to the PCB)
  suits the enclosure/cable-management plan — if edge-parallel exit is needed, check
  for a right-angle sibling in the same family before committing.
- **Rejected:** 3.5 mm pluggable (only ~8 % denser than V1.5 — not worth a connector
  change); 2.54 mm spring-clamp is the right call once the light-load envelope was
  confirmed.

---

## 8. Power tree

Reuse V1.5's front-end and buck; extend for the H7's higher current and analog needs.
- **Input protection:** carry forward series Schottky (reverse-polarity) + TVS +
  PPTC, and apply V1.5's **open** fix here from the start: **D1 → SMCJ30A** (C408374),
  unidirectional 30 V standoff, clamps under the buck's 42 V abs-max.
- **24 V → 5 V:** **LMR33630ADDAR** (C841384) buck, unchanged design (§ root
  `power_supply_design.md`). H723 + PHY + drivers still fit the 3 A budget with margin.
- **5 V → 3.3 V:** the H723 (550 MHz) + PHY draw more than the F411. Two options:
  - **(A) AMS1117-3.3** (simple, as V1.5). At ~0.3–0.4 A load the drop from 5 V is
    ~0.6–0.7 W — acceptable but warm. **OK for first spin.**
  - **(B) Small 3.3 V buck** (e.g. a 2nd LMR33630 or a fixed-3.3 V part) — cooler,
    more efficient, better if total 3V3 load climbs. **Recommended if headroom is
    tight** after tallying PHY + core + logic.
- **Analog rail:** derive **VDDA/VREF+** from 3V3 through a **ferrite + LC** for a
  quiet ADC/DAC reference; optionally an **external 3.0 V reference** (e.g. REF3030,
  SOT-23) into VREF+ for absolute 16-bit ADC accuracy — **optional**, decide by
  accuracy target.
- **Analog-out rail:** per §6.4 — either the 24 V rail (option A, no new rail) or a
  dedicated 12 V (option B).
- **H7 core supplies:** VCAP caps, VDD33USB, VBAT per §4.1 / the datasheet power map.

---

## 9. Preliminary pin budget — LQFP-144 (~114 GPIO)

**Preliminary allocation only** — every assignment must be checked against the H723
datasheet AF tables (RMII, timer-CH, DAC, ADC channel, USART AF pins **overlap** and
must be de-conflicted). This mirrors V1.5's separate `mcu_pinmap.md`; a dedicated
`H723_pinmap.md` gets built in Phase 2.

| Function | Pins | Notes |
|---|---:|---|
| HSE crystal (PH0/PH1) | 2 | 25 MHz, shared clock domain w/ PHY |
| SWD (PA13/PA14) | 2 | debug; SWO optional |
| USB FS (DM/DP + VBUS-sense) | 3 | on-chip PHY; PA11/PA12 + sense |
| Ethernet RMII (9) + nRST + nINT | 11 | must land on valid RMII AF pins |
| Encoders A/B ×5 | 10 | timer encoder-mode CH1/CH2 pairs |
| Digital outputs ×16 | 16 | one contiguous port (BSRR word) |
| Isolated inputs ×8 (→16) | 8–16 | quad optos; internal pull-ups |
| Analog inputs ×4 (→8) | 4–8 | ADC1/2 channels |
| Analog outputs ×2 (DAC) | 2 | PA4/PA5 (DAC1_OUT1/2) |
| RS-485 ×2 (TX/RX/DE each) | 6 | 2 USARTs |
| FDCAN (optional) | 2 | TX/RX |
| I²C expansion (optional) | 2 | carry V1.5's expansion bus |
| Status LEDs | 2–3 | run/link/activity |
| **Total (typical)** | **~70–75** | **of 114 → comfortable headroom** |

Conflict watch-list to resolve in Phase 2: RMII commonly wants PA1/PA2/PA7/PC1/PC4/PC5
which collide with popular timer/ADC pins — pick encoder timers/pins that dodge the
RMII set; PA4/PA5 are DAC-dedicated (don't also demand them for SPI); ensure the
16-output port isn't cannibalized by RMII/FDCAN AF needs.

---

## 10. Parts of record (JLCPCB/LCSC — checked 2026-07-15, re-verify at order)

| Ref | Part | LCSC | Pkg | Role | Notes |
|---|---|---|---|---|---|
| MCU | STM32H723ZGT6 | **C730146** | LQFP-144 | main MCU | ~$6.6; 1 MB/564 KB, 550 MHz |
| PHY | LAN8742A-CZ-TR | **C621424** | QFN-24-EP | Ethernet RMII PHY | ~$0.97; ST-Nucleo reference PHY |
| OUT ×2 | TBD62083APG | **C114143** | SOIC/DIP-18 | 8-ch DMOS sink | 0.4–0.65 V drop, 50 V/500 mA |
| RS485 ×2 | SP3485EN-L/TR | **C8963** | SOIC-8 | RS-485 xceiver | carry-over from V1.5 |
| Buck | LMR33630ADDAR | **C841384** | HSOIC-8 PP | 24→5 V | carry-over from V1.5 |
| LDO | AMS1117-3.3 | (V1.5 ref) | SOT-223 | 5→3.3 V | or a 3.3 V buck (see §8) |
| USB ESD | USBLC6-2SC6 | (common) | SOT-23-6 | USB D± ESD | verify LCSC # |
| Out TVS | SMAJ12A | **C113957** | SMA | analog-out clamp | carry-over |
| In TVS | SMAJ6.0CA | **C466511** | SMA | opto-input ESD | V1.5 open item, baked in |
| In-buck TVS | SMCJ30A | **C408374** | SMC | 24 V input clamp | V1.5 open item, baked in |
| Term (I/O) | DORABO DB141V-2.54-*n*P-GN | **C2898750 (8P)**, C2898744…C2898753 by pole | 2.54 mm | signal I/O spring-clamp | screwless, vertical, 6 A/160 V, 18–26 AWG |
| Term (power in) | 3.81 / 5.08 mm screw | (V1.5-class) | — | 24 V main feed | robust, thick supply wire |
| Encoder rx ×5 | AM26LV32EIDR | (V1.5 ref) | SOIC-16 | diff line rx | carry-over |
| Opto in ×2–4 | TLP291-4 (or TLP293-4) | **C60900** (C113626) | SOP-16 | quad isolated input | 4 ch/pkg; per-input LED kept |
| In resistors | series-R networks | (common) | — | LED/opto current-limit | shrinks input bank |
| In CRD (opt) | constant-current diode | (common) | SOD | wide 5–24 V inputs | only where range is wide |
| Analog op-amp | LM2904/LM358-class | (common) | SOIC-8 | 0–10 V gain | 24 V-rail single-supply |
| RJ45 mag | HR911105A-class | (V1.5 ref) | — | integrated magnetics | carry-over |
| CAN xceiver (opt) | TCAN332/SN65HVD230 | (common) | SOIC-8 | FDCAN | optional bus |
| VREF (opt) | REF3030 | (common) | SOT-23 | 3.0 V ADC ref | optional accuracy |

---

## 11. Open decisions (need user/eng input before schematic)

1. **Terminal blocks** — **decided** (§7): DORABO DB141V-2.54 fixed spring-clamp for
   signal I/O (C2898750 = 8P), larger screw block for the 24 V feed. Remaining: confirm
   vertical vs right-angle entry suits the enclosure; confirm board outline → connector
   count → final I/O count.
2. **Input count** — 8 or 16 opto-isolated inputs on the first spin? (isolation +
   per-channel LED kept via quad TLP291-4 arrays, §6.2 → 2 or 4 packages)
3. **Analog channel counts** — 4 or more analog-in; 2 analog-out confirmed.
4. **Analog-out rail** — 24 V-rail op-amp (no new rail) vs dedicated 12 V (§6.4).
5. **3V3 supply** — AMS1117 (simple) vs 3.3 V buck (cool/efficient) (§8).
6. **Expansion bus** — RS-485 port B and/or FDCAN? Both add small BOM.
7. **USB connector** — USB-C (recommended) vs micro-B.
8. **External VREF** — needed, or is VDDA-referenced 16-bit ADC good enough?
9. **Isolated outputs** — accept non-isolated sink (like V1.5) for this spin? (Full
   opto-isolated 16× is a later variant.)
10. **Power mode** — LDO mode confirmed for spin 1 (SMPS deferred)?

---

## 12. Board impact vs V1.5

- **Removed:** W5500 + its SPI plumbing/crystal; GP8403 I²C DAC + its 24 V feed;
  **single-channel input optos** (16× TLP2309 → 4× quad TLP291-4).
- **Added:** LAN8742A + RJ45 magnetics (net smaller than W5500); USB connector + ESD;
  2× TBD62083; DAC gain op-amp(s); analog-input front-ends; optional 2nd RS-485 /
  FDCAN; external VREF (opt).
- **Net:** silicon count roughly flat or down despite ~2× I/O, because three external
  functions folded into the MCU **and** the input optos went 4:1 (per-input LED +
  isolation kept). Main new area cost is the **16-output driver bank**; the switch to
  **2.54 mm spring terminals** shrinks the connector field ~33% linearly, which is what
  buys the extra I/O on a similar outline.
- **Reused wholesale:** encoder front-end (5× AM26LV32E, per-DB9), buck stage,
  RS-485 DE topology, **opto-isolated input concept + per-channel LED**, protection
  philosophy, 4-layer impedance-controlled stackup for the Ethernet pair.

---

## Document index (this variant)
- `H723_variant_design.md` — this file (design of record for the H723 variant).
- `H723_variant_todo.md` — phased task tracker.
- `H723_pinmap.md` — **(to be created, Phase 2)** full LQFP-144 pin/AF audit.
- Root docs (`STATUS.md`, `power_supply_design.md`, `encoder_input_design.md`,
  `analog_output_design.md`, `mcu_pinmap.md`) — parent V1.5 design + rationale reused here.
