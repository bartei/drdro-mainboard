# STM32F411RET6 (LQFP-64) pin map — verified 2026-07-04

Full pin-function audit against ST datasheet Table 8 (DocID026289). U6 = MCU.
Re-verified against `Netlist_Schematic1_2026-07-04.net` (171 comp / 133 nets).

## Power — all 5 VDD/VSS pairs wired ✓ (was the critical finding, now FIXED)
An earlier export left pins 18 (VSS) / 19 (VDD) unconnected; the current netlist
wires both. Verified:
| VSS | 12(VSSA)✓ 18✓ 31✓ 47✓ 63✓ |
| VDD | 13(VDDA)✓ 19✓ 32✓ 48✓ 64✓ |
VBAT(1)→3V3 ✓, VCAP_1(30)→2.2µF (C22) ✓.

## Encoders — hardware quadrature (TIM CHx), all verified ✓
| Enc | A / B | pins | Timer |
|---|---|---|---|
| ENC1 | PA8/PA9 | 41/42 | TIM1 CH1/CH2 (AF1) |
| ENC2 | PA5/PB3 | 21/55 | TIM2 CH1/CH2 (AF1) |
| ENC3 | PA6/PA7 | 22/23 | TIM3 CH1/CH2 (AF2) |
| ENC4 | PB6/PB7 | 58/59 | TIM4 CH1/CH2 (AF2) |
| ENC5 | PA0/PA1 | 14/15 | TIM5 CH1/CH2 (AF2) |
All 5 on distinct timers, valid CH1/CH2 pairs. FW: ENC5 must use TIM5 (AF2)
not TIM2 (PA0/PA1 are also TIM2 CH1/CH2, but TIM2 = ENC2). Receiver outputs
(AM26LV32E, 3.3V) go direct to MCU — safe for PA0 (TC/3.3V-only).
As built the A/B pairs come off **3 shared quad receivers** (U7=ENC1/2,
U8=ENC3/4, U9=ENC5). The pending 5-per-DB9 + **index-Z** restructure will add
5 Z inputs — land them on the free GPIOs listed below (Z is a plain input).

## Comms
- USART1: PA15 TX (50, AF7), PA10 RX (43, AF7), PA11 DE (44, GPIO) ✓
- I2C1: PB8 SCL (61, AF4), PB9 SDA (62, AF4) ✓ — NO external pull-ups yet (expansion bus)
- SPI2: PB12 NSS / PB13 SCK / PB14 MISO / PB15 MOSI (33-36, AF5) ✓ — **wired to
  W5500** (SCSn/SCLK/MISO/MOSI); INTn→PC6 (37), RSTn→PC7 (38), both pulled up
- SWD: PA13 SWDIO (46), PA14 SWCLK (49) ✓

## Motor (GPIO → ULN2003 U16 → CN6/CN7)
| Sig | pin | note |
|---|---|---|
| M_ENA | PC10 (51) | ✓ shared enable |
| M1_STEP/DIR | PC12/PC11 (53/52) | ✓ |
| M2_STEP/DIR | PC0/PC1 (8/9) | ✓ (was PD2/PB4) |
| M3_STEP/DIR | PC2/PC3 (10/11) | ✓ M3_DIR moved off PC13 → PC3 |
All 7 channels MCU→ULN2003 (U16)→CN6/CN7 verified. STEP pins not on free timer
channels (TIM1-5 used by encoders) → step gen is GPIO/DMA, not HW PWM. ULN2003
slow (~1µs).

## Other I/O
- ISO_IN1-6: PC4,PC5,PB0,PB1,PB2,PB10 (24-29) GPIO in ✓
- Status LED: PA12 (45) → LED3 ✓
- Reset: NRST(7); R10 1.5k pull-up→3V3, D3 SWD-steer, D4 button-steer ✓ (no 100nF cap - optional)
- BOOT0(60): R12 1.5k pull-down→GND, BOOT1 button→3V3 ✓
- Crystal: PH0/PH1 (5/6) ✓

## Free GPIOs (spare) — 11 pins, verified from netlist
PC13 (2), PC14 (3), PC15 (4), PA2 (16), PA3 (17), PA4 (20), PC8 (39), PC9 (40),
PD2 (54), PB4 (56, NJTRST), PB5 (57). (PC0–3 now motors; PC6/PC7 now W5500
INT/RST.) This pool covers the 5 index-Z inputs for the pending encoder
restructure. Avoid PC13 where drive current is needed (backup-domain pin: ≤3mA,
no current source) — fine as a plain input.

## JTAG note
SWD-only: PA15=JTDI→USART1_TX and PB3=JTDO/SWO→ENC2B are repurposed. PB4=NJTRST
is now FREE (M2_DIR moved to PC1). Fine — lose JTAG + SWO trace, keep SWD.
