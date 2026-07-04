# STM32F411RET6 (LQFP-64) pin map — verified 2026-07-04

Full pin-function audit against ST datasheet Table 8 (DocID026289). U6 = MCU.

## Power (CRITICAL FINDING)
LQFP64 has 5 VDD/VSS pairs. Netlist wires only 4 — **pins 18 (VSS) and 19
(VDD) are UNCONNECTED**. Must fix: 18→GND, 19→3V3, +100nF decoupling. Likely
a library symbol missing that pair; verify symbol exposes pins 18/19.
| VSS | 12(VSSA)✓ 18✗ 31✓ 47✓ 63✓ |
| VDD | 13(VDDA)✓ 19✗ 32✓ 48✓ 64✓ |
VBAT(1)→3V3 ✓, VCAP_1(30)→2.2µF ✓.

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

## Comms
- USART1: PA15 TX (50, AF7), PA10 RX (43, AF7), PA11 DE (44, GPIO) ✓
- I2C1: PB8 SCL (61, AF4), PB9 SDA (62, AF4) ✓ — NO external pull-ups yet (expansion bus)
- SPI2: PB12 NSS / PB13 SCK / PB14 MISO / PB15 MOSI (33-36, AF5) ✓ — WIP, not wired
- SWD: PA13 SWDIO (46), PA14 SWCLK (49) ✓

## Motor (GPIO → ULN2003)
| Sig | pin | note |
|---|---|---|
| M_ENA | PC10 | ✓ |
| M1_DIR/STEP | PC11/PC12 | ✓ |
| M2_STEP/DIR | PD2/PB4 | ✓ (PB4=NJTRST, ok for SWD) |
| M3_STEP/DIR | PB5 / **PC13** | ⚠️ PC13 backup-domain: ≤3mA, "no current source" — move M3_DIR to a free GPIO |
STEP pins not on free timer channels (TIM1-5 used by encoders) → step gen is
GPIO/DMA, not HW PWM. ULN2003 slow (~1µs).

## Other I/O
- ISO_IN1-6: PC4,PC5,PB0,PB1,PB2,PB10 (24-29) GPIO in ✓
- Status LED: PA12 (45) → LED3 ✓
- Reset: NRST(7); R10 1.5k pull-up→3V3, D3 SWD-steer, D4 button-steer ✓ (no 100nF cap - optional)
- BOOT0(60): R12 1.5k pull-down→GND, BOOT1 button→3V3 ✓
- Crystal: PH0/PH1 (5/6) ✓

## Free GPIOs (spare)
PC0,PC1,PC2,PC3 (8-11); PA2,PA3,PA4 (16,17,20); PC6,PC7,PC8,PC9 (37-40);
PC14,PC15 (3,4). Use one for M3_DIR relocation.

## JTAG note
SWD-only: PA15=JTDI→USART1_TX, PB3=JTDO/SWO→ENC2B, PB4=NJTRST→M2_DIR all
repurposed. Fine — lose JTAG + SWO trace, keep SWD.
