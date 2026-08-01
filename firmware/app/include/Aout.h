/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Analog outputs — GP8403 dual 12-bit I2C DAC (U20 @ 0x58), 0..4095 => 0..10 V
 * on AO0/AO1 (CN9). Registers per docs/analog_output_design.md: config 0x01
 * (0x11 = 0-10 V range), channel data at 0x02 (VOUT0) / 0x04 (VOUT1), 12-bit
 * value left-justified in two bytes, LOW byte first.
 */
#ifndef DRDRO_AOUT_H
#define DRDRO_AOUT_H

#include <stdint.h>
#include "Ramps.h"

/* Configure the DAC range (0-10 V). Returns 0 ok, -1 if the chip NAKs. */
int AoutInit(void);

/* Write one channel (0|1), raw 12-bit code. Returns 0 ok, -1 on I2C error. */
int AoutWrite(uint8_t ch, uint16_t raw);

/* Push both persisted channel values (shared->aout.raw[]) to the DAC. */
void AoutApply(const rampsSharedData_t *shared);

#endif /* DRDRO_AOUT_H */
