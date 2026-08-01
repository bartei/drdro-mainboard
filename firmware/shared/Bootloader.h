/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Shared contract between the drDRO mainboard V1.5 application and its IAP
 * bootloader. Ported unchanged from the drdro-firmware-f4 baseline: the
 * STM32F411RET6 has the same 512 KB flash (4x16K / 1x64K / 3x128K sectors) and
 * 128 KB RAM as the F411CE, so the whole map carries over 1:1.
 *
 * Flash layout:
 *   sector 0       0x08000000  16 KB   bootloader (never erased)
 *   sector 1       0x08004000  16 KB   settings slot A (see Settings.h)
 *   sector 2       0x08008000  16 KB   settings slot B (ping-pong shadow)
 *   sectors 3..4   0x0800C000  80 KB   reserved (future / recovery)
 *   sector 5       0x08020000 128 KB   Exec    — the app runs here (single build)
 *   sector 6       0x08040000 128 KB   Bank 0  — stored app version
 *   sector 7       0x08060000 128 KB   Bank 1  — stored app version
 *
 * Banking is copy-on-activate: the app is linked once at APP_EXEC_BASE; banks are
 * storage. On boot the bootloader copies the active bank into Exec (if stale) and
 * jumps.
 *
 * Update handshake: the app's `update` command writes BOOT_FLAG = BOOT_MAGIC into a
 * reserved no-init RAM word (top 16 bytes of RAM, carved out by both linker
 * scripts), then hands off — a one-shot "enter the bootloader" request that costs
 * no flash wear. The bootloader clears it once consumed. Persistent boot target
 * lives in Settings.h.
 *
 * Unlike the baseline board, BOOT0 is pulled down here (1.5 k + button), so
 * NVIC_SystemReset cannot land in the ST system ROM. App->bootloader handoff is
 * kept as a jump anyway (same behavior/timing as the proven baseline); the
 * Ethernet update path uses a plain reset (see FwUpdate.c).
 */
#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include <stdint.h>

/* --- flash regions (base + erasable sector index) --- */
#define BL_BASE_ADDR     0x08000000U   /* sector 0: bootloader */
#define BL_SECTOR        0U
#define SETTINGS_BASE    0x08004000U   /* sector 1: settings (Settings.h) */
#define SETTINGS_SECTOR  1U

#define APP_EXEC_BASE    0x08020000U   /* sector 5: app execution region (VTOR here) */
#define APP_EXEC_SECTOR  5U
#define APP_REGION_SIZE  0x20000U      /* 128 KB per region (Exec / banks) */
#define APP_REGION_END   (APP_EXEC_BASE + APP_REGION_SIZE)

#define BANK_COUNT       2U
#define BANK0_BASE       0x08040000U   /* sector 6 */
#define BANK0_SECTOR     6U
#define BANK1_BASE       0x08060000U   /* sector 7 */
#define BANK1_SECTOR     7U

/* Bank n base / sector (n in 0..BANK_COUNT-1). */
#define BANK_BASE(n)     ((n) ? BANK1_BASE   : BANK0_BASE)
#define BANK_SECTOR(n)   ((n) ? BANK1_SECTOR : BANK0_SECTOR)

/* Persistent boot mode (Settings.boot_mode). */
typedef enum { BOOT_MODE_APP = 0, BOOT_MODE_BOOTLOADER = 1 } boot_mode_t;

/* No-init handshake word. Must match the LENGTH reserve in both linker scripts
 * (RAM = 128K - 16 => this address is just past _estack, untouched by stack/heap). */
#define BOOT_FLAG_ADDR   0x2001FFF0U
#define BOOT_MAGIC       0xB00710ADU   /* "boot load" */
#define BOOT_FLAG        (*(volatile uint32_t *)BOOT_FLAG_ADDR)

#endif /* BOOTLOADER_H */
