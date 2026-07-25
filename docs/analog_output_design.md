# Analog outputs — 2× 0–10 V for VFD speed reference

## Goal
Add **two 0–10 V analog outputs** to command spindle/motor RPM on variable-frequency
drives (VFD analog speed reference). Decided **2026-07-10**. **4–20 mA was
considered and dropped** (user, 2026-07-10) — 0–10 V only. Non-isolated (board and
VFD share the cabinet 0 V), cost/JLCPCB-stock prioritized.

## Part of record
**U20 (next free designator — confirm in EDA) = GP8403-F50-NF-EW** (Guestgood /
客益微), ESSOP-10-150mil, **JLCPCB/LCSC C3152007** (~$1.70@1, ~4.9k stock 2026-07-10
— re-verify JLCPCB SMT stock/part-class at order time, changes daily).

Dual 12-bit I²C→analog-voltage DAC: **one chip = both channels**. 0–5 V / 0–10 V
range software-selectable. 0.5% output error (0.2% "H" grade exists if ever needed).
Response time ~100–200 µs. Each VOUT sources up to 20 mA.

### Why this part
- **STM32F411 has no internal DAC** (unlike F410/F407) — an external device is
  required no matter what; this is the tidiest one.
- **24 V-native**: VCC range 10–40 V (nom 12 V, tested at 24 V). Runs straight off
  the board's existing 24 V rail — no new supply, no op-amp gain stage, no
  PWM+filter ripple/settling tuning.
- Dual channel in one small SO-package; cheap and well-stocked on JLCPCB.
- Alternatives rejected: **DAC8760/AD5420** (does 0–10 V *and* 4–20 mA in one chip,
  but ~2–3× cost + extended-part stock risk — overkill once 4–20 mA was dropped);
  **PWM (TIM9 CH1/CH2 = PA2/PA3) + RC filter + op-amp** (cheapest silicon but adds a
  filter+RRO-op-amp stage per channel, ripple/settling trade, more board area than
  the GP8403 for ~the same cost).

## Supply
- **VCC ← 24 V rail.** Add local decoupling: 0.1 µF + 10 µF at the pin. The raw 24 V
  bus carries motor-driver switching noise → add a **series ~10 Ω (or ferrite) + 10 µF
  RC** feeding the DAC VCC to keep that noise off the reference. (5 V/3V3 rails are
  too low — the part needs ≥10 V.)
- **V5V pin**: internal 5 V LDO output — needs an external **≥1 µF** cap to GND
  (datasheet requirement).

## Digital interface (I²C) — DECIDED: shared I²C1 expansion bus (2026-07-10)
- **On the existing I²C1 expansion bus** (PB8 SCL / PB9 SDA), shared with the five
  DB9 connectors' pin 5/9. U20-1 SCLK→SCL, U20-2 SDA→SDA. **No new GPIO used.**
  - Pull-ups: **existing R74 (SCL) / R14 (SDA) 4.7 kΩ → 3V3** — reused, none added.
  - I²C logic high 2.7–5.5 V → 3.3 V MCU drive is in spec.
  - **Address = 0x58** (A0/A1/A2 all strapped to GND). **Reserve 0x58** — no future
    DB9-bus expansion device may reuse it.
  - Trade-off accepted: the AO DAC shares a bus routed out to 5 external connectors,
    so a field short on a DB9 pin 5/9 could disturb DAC comms. Acceptable — the DAC
    holds its last output and a 0–10 V speed ref is not safety-critical here.
- **Rejected alternative:** dedicated bit-bang bus on 2 spare GPIOs (PC8/PC9) for
  isolation from the external bus. Cleaner electrically but costs 2 pins + firmware;
  user chose the shared bus for simplicity (pull-ups already present).

## Analog outputs
- VOUT0 → **AO0**, VOUT1 → **AO1** (as-built net names).
- **Per output**: **1 µF** cap to GND (C42/C43) **+ 12 V unidirectional TVS to GND**
  (**SMAJ12A**, JLCPCB **C113957**, SMA/DO-214AC — same TVS family already used on
  the board; cathode→signal, anode→GND). 12 V standoff clears the 0–10 V signal and
  clamps (~19.9 V) well under the 40 V abs-max. **Cap value per the official (Chinese)
  datasheet pin table: 1 µF on each VOUT, >1 µF on V5V** — as-built 1 µF is correct.
  (The third-party English "StanStrong" translation says 10 µF; it's wrong — official
  source governs.)
- **47 Ω series R** to the terminal for short/surge hardening (R75/R76) — VFD analog
  inputs are high-Z (≥10 kΩ typ) so the divider error is negligible. TVS + cap sit on
  the DAC side, series R between them and the connector.
- **Connector**: **CN9**, 4-pos terminal block, **WJ15EDGRC-3.81-4P** (matches
  existing connectors) — **AO0 / AO1 / GND / GND** (pin4/3 = outputs, pin1/2 = COM).
  Wiring to VFD: AOx → analog-in (AI/VI), COM → analog-common (ACM/GND). Non-isolated:
  COM = board GND.

## Range / resolution / accuracy
0–10 V, 12-bit → **2.44 mV/LSB**, 0.5% total error, 50 ppm/°C, ~200 µs settle.
Far finer than any VFD speed-reference needs.

## Firmware notes
- **Set 0–10 V mode once** at boot: config register (addr byte **0x01**), write
  **0x11** (0x00 = 0–5 V). Optionally persist to on-chip NVM (§3.3.7) so it survives
  power-down.
- **Write a channel**: VOUT0 via command **0x02**, VOUT1 via **0x04**; 12-bit code
  as two bytes (low byte first, low 4 bits of the low byte ignored — i.e. code is
  left-justified). Output: **VOUT = DATA/0xFFF × 10 V**.
- Scale spindle RPM setpoint → 12-bit code per VFD's max-RPM-at-10 V setting.

## Board impact (as-built, `Netlist_Schematic1_2026-07-10.net`)
+1 IC (U20 GP8403), passives: C42/C43 1 µF out, D5/D6 SMAJ12A TVS, R75/R76 47 Ω
series, C45 1 µF V5V (+ VCC decoupling), +1 terminal block (CN9). **0 new MCU GPIO**
(shares I²C1 PB8/PB9; pull-ups R14/R74 reused). Component count 177 → **189**.

## AS-BUILT — verified 2026-07-10 (netlist review)
U20 = GP8403-TC50-EW (C3152007). VCC→VIN (24 V, D2/U2 source); GND + **EP→GND** ✓;
SCLK/SDA→SCL/SDA (I²C1 + DB9 pin5/9 + R74/R14); **A0/A1/A2→GND = addr 0x58**;
V5V→C45(1 µF); VOUT0→AO0, VOUT1→AO1, each C(1 µF)+SMAJ12A(→GND)+47 Ω→CN9
(AO0/AO1/GND/GND). No dangling nets; 3V3/5V/VIN all sourced. **Clean.**
Open (optional): local 0.1 µF + RC/ferrite on VCC to keep 24 V-bus switching noise
off the reference; reserve I²C **0x58** against future DB9-bus devices.
