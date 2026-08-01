/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Flash programming for the IAP bootloader. Targets a single 128 KB region (Exec or a
 * bank) per "program session", plus the settings sector and a bank→Exec copy. Sector 0
 * (bootloader) is never erased. Every word is read-back verified. Design/plan:
 * dualbank_design.md / dualbank_todo.md (D1.2, D1.3).
 */
#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>
#include "Settings.h"

/* --- streaming program session (used by the YMODEM receiver) --- */
/* Begin writing into a region: remember (base, sector), unlock flash, arm a lazy erase
 * (the sector is erased on the first write). */
void flash_program_begin(uint32_t base, uint32_t sector);
/* Program len bytes (multiple of 4) at base+offset; lazily erases the region's sector
 * first. Read-back verifies each word. 0 ok, -1 on error / out-of-region / sector 0. */
int  flash_program_write(uint32_t offset, const uint8_t *data, uint32_t len);
/* Lock flash. Call once after the session (success or failure). */
void flash_program_end(void);

/* --- one-shot helpers --- */
/* Erase a region sector then copy APP_REGION_SIZE bytes from src_base into it. */
int  flash_copy_region(uint32_t dst_sector, uint32_t dst_base, uint32_t src_base);
/* Erase one region sector (Exec or a bank). Refuses sector 0. */
int  flash_erase_sector(uint32_t sector);
/* Ping-pong settings write: prepare *s (bump seq + seal), then erase+program the
 * inactive settings slot (power-fail safe — the previous slot stays intact). */
int  flash_write_settings(settings_t *s);

#endif /* FLASH_H */
