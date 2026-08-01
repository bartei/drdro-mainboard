/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * RS-485 half-duplex transmit for the bootloader. The V1.5 board's SP3485 has DE
 * and /RE tied to PA11 (BoardPins.h): bl_tx() asserts DE, transmits blocking
 * (returns after the TC flag, so the release cannot clip the last stop bit), then
 * releases DE back to receive. Implemented in main.c; used by bl_cli.c and
 * ymodem.c in place of the baseline's bare HAL_UART_Transmit (that board had an
 * auto-direction transceiver).
 */
#ifndef BL_IO_H
#define BL_IO_H

#include <stdint.h>

void bl_tx(const uint8_t *data, uint16_t len);

#endif /* BL_IO_H */
