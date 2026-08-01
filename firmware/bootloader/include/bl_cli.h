/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Bootloader CLI — same wire format as the app's line protocol (key=value lines,
 * terminating empty line, `crc=HH`, optional `*HH` request checksum), but a
 * self-contained implementation (DC5: keep the bootloader isolated/robust; don't
 * couple it to the shipped app protocol). Drives the flash/bank/boot cycle.
 */
#ifndef BL_CLI_H
#define BL_CLI_H

#include "stm32f4xx_hal.h"

/* Run the CLI on the given UART (polled). Returns only via `boot` (jumps to the app)
 * or `reset` (system reset) — i.e. effectively never. */
void bl_cli_run(UART_HandleTypeDef *huart);

#endif /* BL_CLI_H */
