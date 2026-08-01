/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * drDRO mainboard V1.5 IAP bootloader (sector 0 @ 0x08000000). Ported from the
 * drdro-firmware-f4 baseline; board diffs are confined to this file:
 *   - 16 MHz HSE (PLLM=8) instead of 8 MHz (PLLM=4)
 *   - USR_LED on PA12 (active-low) instead of PB12
 *   - RS-485 driver-enable on PA11 (SP3485 DE + /RE tied): asserted around every
 *     transmit via bl_tx() — the baseline board had an auto-direction transceiver
 *
 * Boot decision: consume the one-shot handshake word; read persistent settings
 * (Settings.h). Enter the CLI (LED 2 blinks) if a bootloader entry was requested,
 * the boot mode is "bootloader", or there is no valid app to run. Otherwise ensure
 * the Exec region holds the active bank (copy-on-activate) and jump to it. The
 * handoff is a direct jump, matching the baseline. (BOOT0 is pulled down on this
 * board, so unlike the baseline a reset could not stray into the ST system ROM —
 * the jump is kept for behavioral parity.)
 *
 * Self-contained: HAL for clock + USART + GPIO/flash, polled I/O, SysTick timebase.
 */
#include <string.h>
#include "stm32f4xx_hal.h"
#include "BoardPins.h"
#include "Bootloader.h"
#include "Settings.h"
#include "BlinkCode.h"
#include "flash.h"
#include "ymodem.h"
#include "bl_boot.h"
#include "bl_cli.h"
#include "bl_led.h"
#include "bl_io.h"

static UART_HandleTypeDef huart1;
static int s_hw_inited = 0;

void Error_Handler(void) { __disable_irq(); while (1) { } }

/* 16 MHz HSE -> 100 MHz PLL, same tree as the app so USART1 baud matches exactly.
 * (VCO-in = 16/8 = 2 MHz, VCO = 200 MHz, SYSCLK = 100 MHz.) */
#define CFG_PLLM 8U
#define CFG_PLLN 100U
#define CFG_PLLP 2U
_Static_assert(HSE_VALUE == 16000000U, "board crystal X1 is 16 MHz — check -D HSE_VALUE");
_Static_assert(HSE_VALUE / CFG_PLLM >= 1000000U && HSE_VALUE / CFG_PLLM <= 2000000U,
               "PLL VCO input must be 1..2 MHz");
_Static_assert((uint64_t)HSE_VALUE / CFG_PLLM * CFG_PLLN >= 100000000U &&
               (uint64_t)HSE_VALUE / CFG_PLLM * CFG_PLLN <= 432000000U,
               "PLL VCO output must be 100..432 MHz");
_Static_assert((uint64_t)HSE_VALUE / CFG_PLLM * CFG_PLLN / CFG_PLLP == 100000000U,
               "SYSCLK must be 100 MHz (app and bootloader must agree on baud timing)");

void SystemClock_Config(void) {
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  osc.HSEState = RCC_HSE_ON;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  osc.PLL.PLLM = CFG_PLLM;
  osc.PLL.PLLN = CFG_PLLN;
  osc.PLL.PLLP = RCC_PLLP_DIV2;
  osc.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();

  clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV2;
  clk.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_3) != HAL_OK) Error_Handler();
}

/* HAL timebase tick — no FreeRTOS in the bootloader, so SysTick is ours. */
void SysTick_Handler(void) { HAL_IncTick(); }

/* USART1 pins: PA10=RX, PA15=TX, AF7 + PA11 = RS-485 DE (plain GPIO, idle LOW = RX). */
void HAL_UART_MspInit(UART_HandleTypeDef *h) {
  if (h->Instance != USART1) return;
  GPIO_InitTypeDef g = {0};
  __HAL_RCC_USART1_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  g.Pin = RS485_RX_Pin | RS485_TX_Pin;
  g.Mode = GPIO_MODE_AF_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  g.Alternate = RS485_UART_AF;
  HAL_GPIO_Init(GPIOA, &g);

  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);
  g.Pin = RS485_DE_Pin;
  g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  g.Alternate = 0;
  HAL_GPIO_Init(RS485_DE_GPIO_Port, &g);
}

static void usart1_init(void) {
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK) Error_Handler();
}

/* RS-485 half-duplex TX: assert DE (which also gates /RE, muting our receiver),
 * transmit blocking — HAL_UART_Transmit returns only after the TC flag, i.e. the
 * last stop bit is on the wire — then release DE back to receive. Used by the CLI
 * and the YMODEM receiver (bl_io.h). */
void bl_tx(const uint8_t *data, uint16_t len) {
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_SET);
  HAL_UART_Transmit(&huart1, (uint8_t *)data, len, HAL_MAX_DELAY);
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);
}

/* USR_LED on PA12, active-low (BoardPins.h). The bootloader repeats a 2-blink
 * pattern (BLINK_BOOTLOADER) for as long as it's in CLI mode; the app repeats 1
 * blink. The heartbeat is a non-blocking SysTick FSM serviced from the CLI poll
 * loop. */
static void led_init(void) {
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef g = {0};
  g.Pin = USR_LED_Pin;
  g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(USR_LED_GPIO_Port, &g);
}
static void led_write(int on) {
  HAL_GPIO_WritePin(USR_LED_GPIO_Port, USR_LED_Pin, on ? USR_LED_ON_STATE : USR_LED_OFF_STATE);
}

static uint8_t s_led_code = 0;
void bl_led_set(uint8_t code) { s_led_code = code; }
void bl_led_service(void) {
  static uint32_t due = 0;        /* HAL tick at which the next step is allowed */
  static uint16_t step = 0;       /* 0..2*code-1 = blinks (even=on, odd=off); else gap */
  uint8_t code = s_led_code ? s_led_code : 1U;
  uint32_t now = HAL_GetTick();
  if (due && (int32_t)(now - due) < 0) return;
  if (step < (uint16_t)(code * 2U)) {
    if (step & 1U) { led_write(0); due = now + BLINK_OFF_MS; }
    else           { led_write(1); due = now + BLINK_ON_MS;  }
    step++;
  } else {
    led_write(0); due = now + BLINK_GAP_MS; step = 0;   /* gap, then repeat */
  }
}

/* Bring up clock + flash timebase once (needed for flash copy/erase and HAL_Delay). */
static void hw_init_once(void) {
  if (s_hw_inited) return;
  HAL_Init();
  SystemClock_Config();
  s_hw_inited = 1;
}

/* ---- boot helpers (bl_boot.h) ------------------------------------------- */
int bl_image_valid(uint32_t vec_base) {
  uint32_t sp = *(volatile uint32_t *)vec_base;
  uint32_t pc = *(volatile uint32_t *)(vec_base + 4U);
  if (sp < 0x20000000U || sp > 0x20020000U) return 0;
  if (pc <  APP_EXEC_BASE || pc >= APP_REGION_END) return 0;   /* always Exec-linked */
  return 1;
}

int bl_bank_trusted(uint8_t bank, const settings_t *s) {
  if (bank >= BANK_COUNT) return 0;
  if (!bl_image_valid(BANK_BASE(bank))) return 0;
  if (s && s->bank_crc[bank] != 0U &&
      settings_crc32((const void *)BANK_BASE(bank), APP_REGION_SIZE) != s->bank_crc[bank])
    return 0;                                       /* recorded CRC mismatch → corrupt */
  return 1;
}

int bl_load_bank(uint8_t bank) {
  if (bank >= BANK_COUNT) return -1;
  hw_init_once();                                  /* flash timing needs the clock */
  return flash_copy_region(APP_EXEC_SECTOR, APP_EXEC_BASE, BANK_BASE(bank));
}

void bl_jump_to_exec(void) {
  uint32_t sp = *(volatile uint32_t *)APP_EXEC_BASE;
  uint32_t pc = *(volatile uint32_t *)(APP_EXEC_BASE + 4U);
  __disable_irq();
  SysTick->CTRL = 0U;
  SCB->VTOR = APP_EXEC_BASE;
  __set_MSP(sp);
  __DSB();
  __ISB();
  ((void (*)(void))pc)();
  while (1) { }                                    /* not reached */
}

int bl_boot_app(void) {
  settings_t s;
  int valid = settings_load(&s);
  uint8_t bank = (s.active_bank < BANK_COUNT) ? s.active_bank : 0U;

  /* Copy only when settings select a *trusted* bank that isn't the one already in Exec.
   * Requiring the bank to be trusted means a unit whose active bank is empty (e.g. a
   * factory image seeded straight into Exec, or after the app first writes settings with
   * loaded_bank=0xFF) still runs the valid Exec image instead of dropping to the CLI. */
  int need_copy = valid && (s.loaded_bank != bank) && bl_bank_trusted(bank, &s);

  if (!need_copy && bl_image_valid(APP_EXEC_BASE)) {
    bl_jump_to_exec();                              /* run what's in Exec — no return */
  }
  if (bl_bank_trusted(bank, &s)) {                  /* (re)load the active bank into Exec */
    if (bl_load_bank(bank) != 0) return -1;
    s.loaded_bank = bank;
    flash_write_settings(&s);                       /* prepare() seals + ping-pongs */
    if (bl_image_valid(APP_EXEC_BASE)) bl_jump_to_exec();   /* no return */
  }
  return -1;                                        /* nothing valid → CLI */
}

int main(void) {
  uint32_t req = BOOT_FLAG;
  BOOT_FLAG = 0U;                                  /* consume one-shot request */

  settings_t s;
  settings_load(&s);

  int enter_cli = (req == BOOT_MAGIC) || (s.boot_mode == BOOT_MODE_BOOTLOADER);

  if (!enter_cli) {
    bl_boot_app();                                 /* jumps to the app, or returns on no-valid-app */
  }

  /* --- CLI / update mode --- */
  hw_init_once();
  usart1_init();
  led_init();
  bl_led_set(BLINK_BOOTLOADER);                    /* repeating 2-blink heartbeat */
  bl_cli_run(&huart1);                             /* exits only via `boot`/`reset` */
  for (;;) { }                                     /* not reached */
}
