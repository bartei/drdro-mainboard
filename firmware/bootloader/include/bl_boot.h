/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Boot helpers shared between bl main and the CLI (`boot` command). Implemented in
 * main.c. See dualbank_design.md §Boot logic.
 */
#ifndef BL_BOOT_H
#define BL_BOOT_H

#include <stdint.h>
#include "Settings.h"

/* Vector-table sanity for an image: initial SP in RAM, reset vector in the Exec
 * range. Stored banks hold Exec-linked images, so PC is checked against Exec for
 * both Exec and bank vectors. vec_base = where the vector table is read from. */
int  bl_image_valid(uint32_t vec_base);

/* A bank is trusted if its vectors are sane AND (when a CRC is recorded in settings)
 * its 128 KB region CRC32 matches settings->bank_crc[bank]. */
int  bl_bank_trusted(uint8_t bank, const settings_t *s);

/* Copy a bank (0|1) into the Exec region (erase + copy + verify). 0 ok, -1 on error. */
int  bl_load_bank(uint8_t bank);

/* VTOR=Exec, set MSP, branch to the app. Never returns. */
void bl_jump_to_exec(void);

/* Ensure Exec holds the active bank (copy if stale, persist loaded_bank), then jump.
 * Returns -1 only if there is no valid app to run (caller should enter the CLI). */
int  bl_boot_app(void);

#endif /* BL_BOOT_H */
