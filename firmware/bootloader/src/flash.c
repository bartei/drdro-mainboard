/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Flash programming — see flash.h. F411CE is single-bank, so a failed write can only
 * damage the targeted region (Exec or a bank); the bootloader in sector 0 stays intact.
 * Sector 0 is never erased. A program session targets one 128 KB region and lazily
 * erases its sector on the first write, so a partial image leaves invalid vectors.
 */
#include <string.h>
#include "stm32f4xx_hal.h"
#include "Bootloader.h"
#include "flash.h"

#define WORD_CHUNK 256U   /* copy granularity (bytes) */

/* ---- program session state ---- */
static uint32_t s_base;
static uint32_t s_sector;
static uint8_t  s_erased;

/* Failure forensics for cmd_flash's error response: which phase died
 * (1=erase, 2=program, 3=verify), where, and the HAL error code. */
uint32_t flash_fail_phase;
uint32_t flash_fail_addr;
uint32_t flash_fail_hal;

int flash_erase_sector(uint32_t sector) {
  if (sector == BL_SECTOR) return -1;            /* never erase the bootloader */
  FLASH_EraseInitTypeDef e = {0};
  e.TypeErase    = FLASH_TYPEERASE_SECTORS;
  e.Sector       = sector;
  e.NbSectors    = 1U;
  e.VoltageRange = FLASH_VOLTAGE_RANGE_3;         /* 3.3 V => 32-bit program/erase */
  uint32_t err = 0;
  if (HAL_FLASHEx_Erase(&e, &err) == HAL_OK) return 0;
  flash_fail_phase = 1U;
  flash_fail_addr  = err;                    /* faulty-sector report from HAL */
  flash_fail_hal   = HAL_FLASH_GetError();
  return -1;
}

/* Program + read-back verify a word-aligned span. Flash must be unlocked. */
static int program_verify(uint32_t addr, const uint8_t *data, uint32_t len) {
  for (uint32_t i = 0; i < len; i += 4U) {
    uint32_t w;
    memcpy(&w, data + i, 4U);
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, w) != HAL_OK) {
      flash_fail_phase = 2U; flash_fail_addr = addr + i; flash_fail_hal = HAL_FLASH_GetError();
      return -1;
    }
    if (*(volatile uint32_t *)(addr + i) != w) {
      flash_fail_phase = 3U; flash_fail_addr = addr + i; flash_fail_hal = *(volatile uint32_t *)(addr + i);
      return -1;
    }
  }
  return 0;
}

void flash_program_begin(uint32_t base, uint32_t sector) {
  s_base = base; s_sector = sector; s_erased = 0;
  HAL_FLASH_Unlock();
  /* Clear any stale FLASH_SR error flags before starting: latched garbage
   * (e.g. from a stray write to the flash alias region in a previous life —
   * they survive an app<->bootloader jump) makes the HAL abort the first
   * erase/program with a phantom error. */
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                         FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
}

void flash_program_end(void) {
  HAL_FLASH_Lock();
}

int flash_program_write(uint32_t offset, const uint8_t *data, uint32_t len) {
  if (len & 3U) return -1;                         /* word-aligned only */
  uint32_t addr = s_base + offset;
  if (addr < s_base || addr + len > s_base + APP_REGION_SIZE) return -1;
  if (!s_erased) {
    if (flash_erase_sector(s_sector)) return -1;
    s_erased = 1;
  }
  return program_verify(addr, data, len);
}

int flash_copy_region(uint32_t dst_sector, uint32_t dst_base, uint32_t src_base) {
  flash_program_begin(dst_base, dst_sector);
  int rc = 0;
  for (uint32_t off = 0; off < APP_REGION_SIZE && rc == 0; off += WORD_CHUNK)
    rc = flash_program_write(off, (const uint8_t *)(src_base + off), WORD_CHUNK);
  flash_program_end();
  return rc;
}

int flash_write_settings(settings_t *s) {
  int slot = settings_prepare(s);                 /* bump seq + seal; pick inactive slot */
  HAL_FLASH_Unlock();
  /* Clear any stale FLASH_SR error flags before starting: latched garbage
   * (e.g. from a stray write to the flash alias region in a previous life —
   * they survive an app<->bootloader jump) makes the HAL abort the first
   * erase/program with a phantom error. */
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                         FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
  int rc = flash_erase_sector(SETTINGS_SLOT_SECTOR(slot));
  if (rc == 0) rc = program_verify(SETTINGS_SLOT_BASE(slot), (const uint8_t *)s, sizeof(*s));
  HAL_FLASH_Lock();
  return rc;
}
