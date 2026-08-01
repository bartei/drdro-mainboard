/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Minimal YMODEM receiver for the IAP bootloader — self-contained (no libs), polled
 * USART, CRC-16 mode only. Receives a single file and streams it to flash via flash.c.
 * Design/plan: bootloader_todo.md Phase B3; protocol_design.md Part B.
 */
#ifndef YMODEM_H
#define YMODEM_H

#include <stdint.h>
#include "stm32f4xx_hal.h"

enum {
  YM_OK         =  0,
  YM_TIMEOUT    = -1,   /* sender never started / stalled */
  YM_CANCELLED  = -2,   /* sender sent CAN */
  YM_FLASH_ERR  = -3    /* erase/program/verify failed */
};

/* Receive one file over YMODEM and program it into the flash region (base, sector)
 * — an app bank or the Exec region. On success writes the reported file size to
 * *out_size (may be NULL). Returns one of the YM_* codes. */
int ymodem_receive(UART_HandleTypeDef *huart, uint32_t base, uint32_t sector,
                   uint32_t *out_size);

#endif /* YMODEM_H */
