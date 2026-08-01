/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * App-side persistent settings (flash sectors 1/2 ping-pong). Maps the shared
 * settings image (../shared/Settings.h, "DRO2" layout) to/from the live register
 * block, and exposes the boot-bank selector. Bootloader fields (active/loaded
 * bank, boot mode, bank CRCs) are preserved on save.
 *
 * A flash write stalls the single-bank flash bus, but the motion ISR runs from
 * RAM (with a RAM vector table), so step generation continues during a save.
 */
#ifndef SETTINGSSTORE_H
#define SETTINGSSTORE_H

#include <stdint.h>
#include "Ramps.h"

/* Load persisted settings into the live block (only if the flash image is valid). */
void SettingsApply(rampsSharedData_t *shared);

/* Persist the live block's parameters to flash (preserving bootloader fields).
 * Returns 0 on success, -1 on flash error. */
int  SettingsSave(const rampsSharedData_t *shared);

/* Reload the live block's parameters from flash. Returns 1 if a valid image was
 * applied, 0 if none (live values left unchanged in that case). */
int  SettingsLoad(rampsSharedData_t *shared);

/* Set the persistent active bank (0|1) for the next boot. 0 ok, -1 on error. */
int  SettingsBankSet(uint8_t bank);

/* Record a freshly-written bank's region CRC AND select it as active, in one
 * settings save (used by the Ethernet update commit). 0 ok, -1/-... on error. */
int  SettingsCommitBank(uint8_t bank, uint32_t regionCrc);

/* Current persisted active bank (0|1), or 0 if settings are invalid. */
uint8_t SettingsActiveBank(void);

#endif /* SETTINGSSTORE_H */
