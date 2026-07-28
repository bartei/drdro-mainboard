# drDRO mainboard — STM32H723 variant — todo (LIVE tracker)

> Detail + rationale in `H723_variant_design.md`. Scope: STM32H723 + native
> Ethernet (LAN8742A RMII, no W5500) + native USB FS + ≥16 digital outputs + direct
> ADC/DAC analog I/O + denser 3.5 mm terminal blocks. Reuse V1.5 encoder front-end,
> buck, RS-485, opto inputs. Parts/stock checked 2026-07-15 — re-verify at order.

## Phase 0 — Scope & decisions (do first)
- [x] Pick MCU: STM32H723ZGT6, LQFP-144 (C730146) — headroom over LQFP-100
- [x] Ethernet approach: integrated MAC + external RMII PHY (LAN8742A), drop W5500
- [x] USB approach: on-chip FS PHY (device: CDC/DFU) — HS/ULPI out of scope
- [x] Analog: internal DAC×2 (+op-amp gain) out, internal ADC in — drop GP8403
- [x] Outputs: 2× TBD62083APG DMOS sink = 16 channels (C114143)
- [x] Terminals: **DORABO DB141V-2.54 fixed spring-clamp** for signal I/O (C2898750=8P,
      2P–30P family; screwless, vertical, 6 A/160 V, 18–26 AWG). ~33% denser than 3.81 mm.
      24 V power input stays larger-pitch screw block
- [ ] Confirm DB141V vertical entry suits enclosure (else find right-angle sibling)
- [ ] Confirm board outline + connector count → final I/O count (user)
- [x] Inputs: **keep opto isolation + per-channel LED**, compact via **quad opto arrays
      TLP291-4** (C60900; 4 ch/pkg → 16 inputs = 4 pkgs). Higher-CTR alt TLP293-4 (C113626)
- [ ] Decide input count 8 vs 16; analog-in count 4 vs 8 (user)
- [ ] Decide analog-out rail: 24 V op-amp vs dedicated 12 V (§6.4)
- [ ] Decide 3V3 supply: AMS1117 vs 3.3 V buck (§8)
- [ ] Decide expansion: RS-485 port B and/or FDCAN? (user)
- [ ] Decide USB connector: USB-C vs micro-B; external VREF yes/no

## Phase 1 — Architecture freeze
- [ ] Lock final I/O counts (outputs / inputs / analog-in / analog-out)
- [ ] Lock connector map (which functions on which terminal strips, shared-COM plan)
- [ ] Lock power tree (rails: 24 V, 5 V, 3V3, VDDA, analog-out rail)
- [ ] Confirm board outline vs V1.5 (target: similar footprint, denser I/O)
- [ ] BOM v0 with LCSC #s + live stock/part-class recheck

## Phase 2 — MCU pinout audit (→ create `H723_pinmap.md`)
- [ ] Map RMII 9 signals to valid AF pins (REF_CLK/MDC/MDIO/CRS_DV/RXD0-1/TXD0-1/TX_EN)
- [ ] Assign 5 encoders to timer encoder-mode CH1/CH2 pairs (avoid RMII pins)
- [ ] Assign motor STEP pins to timer-PWM-capable GPIOs (HW step-gen — H7 has spare timers)
- [ ] Place 16 digital outputs on one contiguous port (BSRR word: PORTE or PORTD)
- [ ] Assign DAC PA4/PA5; pick ADC1/2 channels for analog inputs (no AF clash)
- [ ] Assign 2× USART for RS-485 (+ DE GPIOs); optional FDCAN pins
- [ ] Assign USB DM/DP (PA11/PA12) + VBUS-sense; SWD PA13/PA14; HSE PH0/PH1
- [ ] Verify no double-booked pins; document full LQFP-144 map + free-pin pool

## Phase 3 — Power schematic
- [ ] Input protection: SMCJ30A TVS (C408374) + Schottky + PPTC (V1.5 open item baked in)
- [ ] 24→5 V LMR33630 buck (carry V1.5 design, re-verify FB divider)
- [ ] 5→3.3 V: AMS1117 or 3.3 V buck per Phase 0 decision
- [ ] H7 power map: all VDD + VDDA/VREF+ + VCAP caps + VBAT + VDD33USB per datasheet/AN5342
- [ ] Analog rail: ferrite/LC to VDDA; optional REF3030 external VREF
- [ ] Analog-out rail per decision (24 V or 12 V)

## Phase 4 — Ethernet schematic (LAN8742A)
- [ ] LAN8742A RMII wiring per Microchip schematic checklist + ST reference
- [ ] 25 MHz crystal at PHY; 50 MHz REF_CLK out → 100 Ω → MCU RMII_REF_CLK
- [ ] Address/mode/auto-neg straps; nRST + nINT GPIOs; per-pin decoupling + 1.2 V cap
- [ ] Integrated-magnetics RJ45 (HR911105A-class); TX±/RX± 100 Ω diff pairs
- [ ] Link/act LEDs

## Phase 5 — USB schematic
- [ ] Connector (USB-C w/ CC pulldowns, or micro-B); D+/D− to on-chip FS PHY
- [ ] USBLC6-2SC6 ESD on D±; VBUS divider → VBUS-sense GPIO (no back-power)
- [ ] VDD33USB decoupling; 48 MHz clock path (PLL off HSE)

## Phase 6 — Digital I/O schematic
- [ ] 2× TBD62083APG: 16 GPIO in → 16 sink out → terminals; COM → field 24 V (clamp)
- [ ] Motor STEP/DIR/ENA assigned within the 16 (STEP on timer pins)
- [ ] Isolated inputs ×8/16: quad TLP291-4 (or TLP293-4); field side terminal→series R→
      indicator LED→opto LED→field COM; board side open-collector→GPIO + pull-up + RC
- [ ] Size series R to field "on" voltage (~3–5 mA); CRD per channel for wide 5–24 V inputs
- [ ] Keep per-channel indicator LED (in series, field-side — no board-side power)
- [ ] Series-R networks to shrink the bank; optional field-line TVS + reverse-polarity diode
- [ ] Output field-side TVS on exposed channels (optional per channel)

## Phase 7 — Analog I/O schematic
- [ ] Analog out ×2: DAC PA4/PA5 → op-amp gain ≈3.03 → 0–10 V
- [ ] Per output: 1 µF + SMAJ12A TVS (C113957) + 47 Ω series → terminal (V1.5 recipe)
- [ ] Op-amp single-supply zero: output pulldown (24 V-rail option)
- [ ] Analog in ×4: divider 0–10 V→0–3 V + RC filter + clamp
- [ ] Buffer analog-in via internal op-amp (low source-Z into 16-bit ADC)

## Phase 8 — Encoder + RS-485 (carry-over)
- [ ] Port V1.5 encoder front-end: 5× AM26LV32E, one per DB9, A/B, 2.2k bias, no term
- [ ] RS-485 port A: SP3485EN + GPIO-DE + 680 Ω bias + 120 Ω term (V1.5 topology)
- [ ] RS-485 port B (optional) + FDCAN (optional) per Phase 0
- [ ] I²C expansion bus (optional carry-over) w/ pull-ups

## Phase 9 — Layout
- [ ] 4-layer, JLC04161H-7628 impedance stackup (reuse V1.5); Inner1 = solid GND
- [ ] Ethernet TX±/RX± 100 Ω diff pairs, short, over unbroken GND
- [ ] USB D± ~90 Ω pair, short, ESD at connector
- [ ] H7 decoupling: 100 nF per VDD pin + VCAP caps tight to pins
- [ ] Buck hot loop tight; analog section away from buck/PHY/switching outputs
- [ ] 16-output bank + terminals along board edge; shared-COM strips
- [ ] Crystal caps at MCU + PHY; mounting holes (consider 4 on a ~178 mm board)

## Phase 10 — Review & fab prep
- [ ] Netlist review (dangling nets, rails, EPs, no shorts) — same rigor as V1.5
- [ ] Adversarial + full schematic/PCB review pass
- [ ] Re-verify JLCPCB stock/part-class for every LCSC # at order time
- [ ] Order-time checklist: 4-layer + impedance control + stackup confirm
- [ ] Bring-up plan: rails → SWD → USB enum → PHY link/ping → encoders → I/O → analog

## Phase 11 — Firmware bring-up notes (parallel)
- [ ] CubeMX/LL project: 550 MHz clock tree, HSE 25 MHz, USB 48 MHz, ETH 50 MHz
- [ ] LwIP over integrated MAC + LAN8742A; USB-CDC/DFU device
- [ ] 5× HW quadrature encoders + HW-PWM step-gen (now possible on H7)
- [ ] ADC (16-bit, DMA) + DAC (0–10 V scaling/calibration); RS-485 DE via USART TC
