/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * App-side persistent settings — see SettingsStore.h. Ported from the
 * drdro-firmware-f4 baseline; the field mapping covers the V1.5 payload
 * (5 scales + dir flags, analog outputs, network config, input debounce).
 */
#include "stm32f4xx_hal.h"
#include "SettingsStore.h"
#include "Settings.h"     /* shared layout */
#include "Bootloader.h"
#include "Scales.h"       /* SCALES_COUNT */

/* The shared image and the live block must agree on the scale count. */
_Static_assert(SETTINGS_SCALES == SCALES_COUNT, "Settings.h SETTINGS_SCALES != Scales.h SCALES_COUNT");

static void shared_to_settings(const rampsSharedData_t *sh, settings_t *s) {
  for (int i = 0; i < SCALES_COUNT; i++) {
    s->scale_num[i]    = sh->scales[i].syncRatioNum;
    s->scale_den[i]    = sh->scales[i].syncRatioDen;
    s->scale_sync[i]   = sh->scales[i].syncEnable;
    s->scale_filter[i] = sh->scales[i].filterValue;
    s->scale_dir[i]    = sh->scales[i].dirInvert;
  }
  s->servo_max   = sh->servo.maxSpeed;
  s->servo_acc   = sh->servo.acceleration;
  s->servo_jog   = sh->servo.jogSpeed;
  s->servo_index = sh->servo.indexSpeed;
  s->servo_mode  = sh->fastData.servoMode;
  for (int i = 0; i < (int)SETTINGS_AOUTS; i++) s->aout_raw[i] = sh->aout.raw[i];
  s->net_dhcp        = (uint8_t)sh->net.dhcp;
  s->din_debounce_ms = (uint8_t)sh->din.debounceMs;
  s->net_port        = sh->net.port;
  for (int i = 0; i < 4; i++) {
    s->net_ip[i]   = sh->net.cfgIp[i];
    s->net_mask[i] = sh->net.cfgMask[i];
    s->net_gw[i]   = sh->net.cfgGw[i];
  }
  s->com_baud = sh->comBaud;
}

static void settings_to_shared(const settings_t *s, rampsSharedData_t *sh) {
  for (int i = 0; i < SCALES_COUNT; i++) {
    sh->scales[i].syncRatioNum = s->scale_num[i];
    sh->scales[i].syncRatioDen = s->scale_den[i];
    sh->scales[i].syncEnable   = s->scale_sync[i];
    sh->scales[i].filterValue  = s->scale_filter[i];
    sh->scales[i].dirInvert    = s->scale_dir[i];
  }
  sh->servo.maxSpeed     = s->servo_max;
  sh->servo.acceleration = s->servo_acc;
  sh->servo.jogSpeed     = s->servo_jog;
  sh->servo.indexSpeed   = s->servo_index;
  sh->fastData.servoMode = s->servo_mode;
  for (int i = 0; i < (int)SETTINGS_AOUTS; i++) sh->aout.raw[i] = s->aout_raw[i];
  sh->net.dhcp        = s->net_dhcp;
  sh->din.debounceMs  = s->din_debounce_ms;
  sh->net.port        = s->net_port;
  for (int i = 0; i < 4; i++) {
    sh->net.cfgIp[i]   = s->net_ip[i];
    sh->net.cfgMask[i] = s->net_mask[i];
    sh->net.cfgGw[i]   = s->net_gw[i];
  }
  sh->comBaud = s->com_baud;
}

/* Erase one settings slot's sector and program the struct there (word-verified). */
static int write_slot(int slot, const settings_t *s) {
  uint32_t base = SETTINGS_SLOT_BASE(slot);
  HAL_FLASH_Unlock();
  /* Clear any stale FLASH_SR error flags before starting: latched garbage
   * (e.g. from a stray write to the flash alias region in a previous life —
   * they survive an app<->bootloader jump) makes the HAL abort the first
   * erase/program with a phantom error. */
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                         FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
  FLASH_EraseInitTypeDef e = {0};
  e.TypeErase    = FLASH_TYPEERASE_SECTORS;
  e.Sector       = SETTINGS_SLOT_SECTOR(slot);
  e.NbSectors    = 1U;
  e.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  uint32_t err = 0;
  int rc = (HAL_FLASHEx_Erase(&e, &err) == HAL_OK) ? 0 : -1;
  const uint32_t *w = (const uint32_t *)s;
  for (uint32_t i = 0; rc == 0 && i < sizeof(*s) / 4U; i++) {
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, base + i * 4U, w[i]) != HAL_OK) { rc = -1; break; }
    if (*(volatile uint32_t *)(base + i * 4U) != w[i]) { rc = -1; break; }
  }
  HAL_FLASH_Lock();
  return rc;
}

/* Ping-pong write to the inactive slot (prepare bumps seq + seals). Interrupts stay
 * ENABLED: the flash bus stall freezes only flash-resident code, while the motion ISR
 * (relocated to RAM, with a RAM vector table) keeps generating steps throughout. */
static int save_struct(settings_t *s) {
  int slot = settings_prepare(s);
  return write_slot(slot, s);
}

void SettingsApply(rampsSharedData_t *shared) {
  settings_t s;
  if (settings_load(&s)) settings_to_shared(&s, shared);
}

int SettingsLoad(rampsSharedData_t *shared) {
  settings_t s;
  if (!settings_load(&s)) return 0;
  settings_to_shared(&s, shared);
  return 1;
}

int SettingsSave(const rampsSharedData_t *shared) {
  settings_t s;
  settings_load(&s);                 /* preserve bootloader fields (active/loaded/mode/crc) */
  shared_to_settings(shared, &s);
  return save_struct(&s);            /* prepare() bumps seq + seals */
}

int SettingsCommitBank(uint8_t bank, uint32_t regionCrc) {
  if (bank >= BANK_COUNT) return -1;
  settings_t s;
  settings_load(&s);
  s.bank_crc[bank] = regionCrc;      /* one save: CRC recorded AND bank selected */
  s.active_bank = bank;
  return save_struct(&s);
}

int SettingsBankSet(uint8_t bank) {
  if (bank >= BANK_COUNT) return -1;
  settings_t s;
  settings_load(&s);
  s.active_bank = bank;
  return save_struct(&s);
}

uint8_t SettingsActiveBank(void) {
  settings_t s;
  return settings_load(&s) ? s.active_bank : 0U;
}
