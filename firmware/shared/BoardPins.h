/**
 * BoardPins.h — drDRO mainboard V1.5 pin map (STM32F411RET6, LQFP-64).
 *
 * Authoritative source: the board netlist (Netlist_Schematic1_2026-07-10.net,
 * U6 = STM32F411RET6 / LCSC C94355), cross-checked against ../docs/mcu_pinmap.md.
 *
 * DO NOT copy pin assignments from ../../drdro-firmware-f4 — that project targets
 * a different board (STM32F411CEU6, LQFP-48) and several pins moved. Notably its
 * user LED is on PB12, which on THIS board is the W5500 chip-select.
 *
 * Package pin numbers are given so each line can be traced back to the netlist.
 */
#ifndef __BOARD_PINS_H
#define __BOARD_PINS_H

/* ------------------------------------------------------------------------- */
/* Status LED (LED3)                                                          */
/*                                                                            */
/* Net $1N6826: LED3 cathode -> PA12 (pin 45); LED3 anode -> R13 -> 3V3.       */
/* => ACTIVE LOW. Drive the pin LOW to light the LED.                         */
/* ------------------------------------------------------------------------- */
#define USR_LED_GPIO_Port        GPIOA
#define USR_LED_Pin              GPIO_PIN_12
#define USR_LED_ON_STATE         GPIO_PIN_RESET
#define USR_LED_OFF_STATE        GPIO_PIN_SET

/* ------------------------------------------------------------------------- */
/* W5500 Ethernet (U19) — SPI2, AF5                                           */
/*                                                                            */
/* SCSn is driven as a software GPIO (SPI2 SSM=1/SSI=1), NOT hardware NSS: the */
/* W5500 needs SCSn asserted for the whole variable-length (VDM) transaction.  */
/* See docs/STATUS.md "FIRMWARE notes" and docs/board_review_todo.md.          */
/* ------------------------------------------------------------------------- */
#define W5500_SPI                SPI2
#define W5500_SPI_GPIO_Port      GPIOB
#define W5500_SCK_Pin            GPIO_PIN_13   /* pin 34, SPI_SCK  -> U19-33 */
#define W5500_MISO_Pin           GPIO_PIN_14   /* pin 35, SPI_MISO -> U19-34 */
#define W5500_MOSI_Pin           GPIO_PIN_15   /* pin 36, SPI_MOSI -> U19-35 */
#define W5500_SPI_AF             GPIO_AF5_SPI2

#define W5500_CS_GPIO_Port       GPIOB
#define W5500_CS_Pin             GPIO_PIN_12   /* pin 33, SPI_NSS  -> U19-32 (R72 10k pull-up) */

#define W5500_RST_GPIO_Port      GPIOC
#define W5500_RST_Pin            GPIO_PIN_7    /* pin 38, SPI_RST  -> U19-37 (R70 pull-up), active low */

#define W5500_INT_GPIO_Port      GPIOC
#define W5500_INT_Pin            GPIO_PIN_6    /* pin 37, SPI_INT  -> U19-36 (R71 pull-up), active low */

/* ------------------------------------------------------------------------- */
/* RS-485 host link (U5 = SP3485EN) — USART1, AF7                             */
/*                                                                            */
/* No hardware driver-enable on the F411: DE is a plain GPIO, asserted before  */
/* TX and released in the USART TC interrupt.                                 */
/* ------------------------------------------------------------------------- */
#define RS485_UART               USART1
#define RS485_TX_GPIO_Port       GPIOA
#define RS485_TX_Pin             GPIO_PIN_15   /* pin 50, UART_TX -> U5-4 (DI) */
#define RS485_RX_GPIO_Port       GPIOA
#define RS485_RX_Pin             GPIO_PIN_10   /* pin 43, UART_RX -> U5-1 (RO) */
#define RS485_UART_AF            GPIO_AF7_USART1

#define RS485_DE_GPIO_Port       GPIOA
#define RS485_DE_Pin             GPIO_PIN_11   /* pin 44, UART_DE -> U5-2 (/RE) + U5-3 (DE) */

/* ------------------------------------------------------------------------- */
/* Encoders — hardware quadrature, 5 axes on 5 distinct timers.                */
/*                                                                            */
/* NOTE ENC5 must use TIM5 (AF2): PA0/PA1 are also TIM2 CH1/CH2, but TIM2 =    */
/* ENC2. The AM26LV32E receivers land A+ on the B input pin (consistent across */
/* all five channels), so raw counts run backwards — compensated once in       */
/* Scales.c by inverting the TI1 capture polarity (see initScaleTimer).        */
/*   ENC1 A/B = PA8/PA9  (41/42)  TIM1 CH1/CH2 (AF1)   U7  = J1                */
/*   ENC2 A/B = PA5/PB3  (21/55)  TIM2 CH1/CH2 (AF1)   U8  = J2                */
/*   ENC3 A/B = PA6/PA7  (22/23)  TIM3 CH1/CH2 (AF2)   U9  = J3                */
/*   ENC4 A/B = PB6/PB7  (58/59)  TIM4 CH1/CH2 (AF2)   U10 = J4                */
/*   ENC5 A/B = PA0/PA1  (14/15)  TIM5 CH1/CH2 (AF2)   U11 = J5                */
/* ------------------------------------------------------------------------- */
#define ENC1_GPIO_Port           GPIOA
#define ENC1_A_Pin               GPIO_PIN_8    /* pin 41, TIM1_CH1 */
#define ENC1_B_Pin               GPIO_PIN_9    /* pin 42, TIM1_CH2 */
#define ENC1_AF                  GPIO_AF1_TIM1

#define ENC2_A_GPIO_Port         GPIOA
#define ENC2_A_Pin               GPIO_PIN_5    /* pin 21, TIM2_CH1 */
#define ENC2_B_GPIO_Port         GPIOB
#define ENC2_B_Pin               GPIO_PIN_3    /* pin 55, TIM2_CH2 */
#define ENC2_AF                  GPIO_AF1_TIM2

#define ENC3_GPIO_Port           GPIOA
#define ENC3_A_Pin               GPIO_PIN_6    /* pin 22, TIM3_CH1 */
#define ENC3_B_Pin               GPIO_PIN_7    /* pin 23, TIM3_CH2 */
#define ENC3_AF                  GPIO_AF2_TIM3

#define ENC4_GPIO_Port           GPIOB
#define ENC4_A_Pin               GPIO_PIN_6    /* pin 58, TIM4_CH1 */
#define ENC4_B_Pin               GPIO_PIN_7    /* pin 59, TIM4_CH2 */
#define ENC4_AF                  GPIO_AF2_TIM4

#define ENC5_GPIO_Port           GPIOA
#define ENC5_A_Pin               GPIO_PIN_0    /* pin 14, TIM5_CH1 */
#define ENC5_B_Pin               GPIO_PIN_1    /* pin 15, TIM5_CH2 */
#define ENC5_AF                  GPIO_AF2_TIM5

/* ------------------------------------------------------------------------- */
/* Motor STEP/DIR/ENA -> ULN2003 (U18) -> CN6/CN7.                            */
/*                                                                            */
/* TIM1-5 are all taken by the encoders, so step generation is GPIO based     */
/* (TIM9 ISR). The ULN2003 inverts and its outputs are open-collector with a  */
/* weak pull-up chain — confirm polarity at the connector with `dout` before   */
/* trusting motion direction. Only M1 is driven by the motion core; M2/M3 are  */
/* configured idle for bring-up via `dout`.                                   */
/* ------------------------------------------------------------------------- */
#define MOTOR_GPIO_Port          GPIOC
#define M_ENA_Pin                GPIO_PIN_10   /* pin 51, shared enable */
#define M1_STEP_Pin              GPIO_PIN_12   /* pin 53 */
#define M1_DIR_Pin               GPIO_PIN_11   /* pin 52 */
#define M2_STEP_Pin              GPIO_PIN_0    /* pin 8  */
#define M2_DIR_Pin               GPIO_PIN_1    /* pin 9  */
#define M3_STEP_Pin              GPIO_PIN_2    /* pin 10 */
#define M3_DIR_Pin               GPIO_PIN_3    /* pin 11 */

/* ------------------------------------------------------------------------- */
/* Opto-isolated inputs (TLP2309, open-collector, sink to GND when active).   */
/* No external pull-ups on the board — enable the STM32 internal pull-ups.    */
/* ------------------------------------------------------------------------- */
#define ISO_IN1_GPIO_Port        GPIOC
#define ISO_IN1_Pin              GPIO_PIN_4    /* pin 24 */
#define ISO_IN2_GPIO_Port        GPIOC
#define ISO_IN2_Pin              GPIO_PIN_5    /* pin 25 */
#define ISO_IN3_GPIO_Port        GPIOB
#define ISO_IN3_Pin              GPIO_PIN_0    /* pin 26 */
#define ISO_IN4_GPIO_Port        GPIOB
#define ISO_IN4_Pin              GPIO_PIN_1    /* pin 27 */
#define ISO_IN5_GPIO_Port        GPIOB
#define ISO_IN5_Pin              GPIO_PIN_2    /* pin 28 */
#define ISO_IN6_GPIO_Port        GPIOB
#define ISO_IN6_Pin              GPIO_PIN_10   /* pin 29 */

/* ------------------------------------------------------------------------- */
/* I2C1 expansion bus + GP8403 analog-out DAC (U20), AF4.                     */
/* External 4.7k pull-ups R74/R14. The bus is also exported on DB9 pins 5/9   */
/* of all five encoder connectors — address 0x58 is reserved for the DAC.     */
/* ------------------------------------------------------------------------- */
#define I2C1_GPIO_Port           GPIOB
#define I2C1_SCL_Pin             GPIO_PIN_8    /* pin 61 */
#define I2C1_SDA_Pin             GPIO_PIN_9    /* pin 62 */
#define I2C1_AF                  GPIO_AF4_I2C1
#define GP8403_I2C_ADDR          (0x58U << 1)  /* HAL 8-bit address */

/* SWD only (JTAG pins repurposed): SWDIO = PA13 (46), SWCLK = PA14 (49).
 * HSE crystal X1 = 16 MHz on PH0/PH1 (5/6). BOOT0 (60) pulled down, button. */

#endif /* __BOARD_PINS_H */
