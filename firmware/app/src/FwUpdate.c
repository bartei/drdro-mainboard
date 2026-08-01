/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Firmware update over Ethernet — see FwUpdate.h.
 *
 * The flash session (lazy erase on first write, word program + read-back
 * verify, bounds-checked to one 128K region) is lifted from the bootloader's
 * flash.c so the two sides behave identically. The stream CRC is computed as
 * bytes arrive, BEFORE the 0xFF word padding, so it matches the host's CRC of
 * the .bin file exactly.
 */
#include <string.h>
#include "cmsis_os2.h"
#include "FwUpdate.h"
#include "Net.h"
#include "Bootloader.h"
#include "Settings.h"        /* settings_crc32 */
#include "SettingsStore.h"
#include "socket.h"

#define FW_SOCKET          2U
#define FW_TIMEOUT_MS      10000U
#define FW_POLL_MS         5U
#define FW_FLAG_ARMED      0x01U

static rampsSharedData_t *sShared = NULL;
static osThreadId_t sTask = NULL;

static volatile fw_state_t sState = FW_IDLE;
static const char *sReason = "";
static uint8_t  sBank = 0;
static uint32_t sSize = 0, sCrcWant = 0;
static volatile uint32_t sReceived = 0;

/* ---- flash program session (mirrors bootloader/src/flash.c) --------------- */
static uint32_t s_base, s_sector;
static uint8_t  s_erased;

static int fwEraseSector(uint32_t sector)
{
  if (sector == BL_SECTOR) return -1;            /* never the bootloader */
  FLASH_EraseInitTypeDef e = {0};
  e.TypeErase    = FLASH_TYPEERASE_SECTORS;
  e.Sector       = sector;
  e.NbSectors    = 1U;
  e.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  uint32_t err = 0;
  return (HAL_FLASHEx_Erase(&e, &err) == HAL_OK) ? 0 : -1;
}

static void fwFlashBegin(uint32_t base, uint32_t sector)
{
  s_base = base; s_sector = sector; s_erased = 0;
  HAL_FLASH_Unlock();
  /* Clear any stale FLASH_SR error flags before starting: latched garbage
   * (e.g. from a stray write to the flash alias region in a previous life —
   * they survive an app<->bootloader jump) makes the HAL abort the first
   * erase/program with a phantom error. */
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                         FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
}

static void fwFlashEnd(void) { HAL_FLASH_Lock(); }

static int fwFlashWrite(uint32_t offset, const uint8_t *data, uint32_t len)
{
  if (len & 3U) return -1;                       /* word-aligned only */
  uint32_t addr = s_base + offset;
  if (addr < s_base || addr + len > s_base + APP_REGION_SIZE) return -1;
  if (!s_erased) {
    if (fwEraseSector(s_sector)) return -1;      /* lazy: first write erases */
    s_erased = 1;
  }
  for (uint32_t i = 0; i < len; i += 4U) {
    uint32_t w;
    memcpy(&w, data + i, 4U);
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, w) != HAL_OK) return -1;
    if (*(volatile uint32_t *)(addr + i) != w) return -1;
  }
  return 0;
}

/* ---- receive state --------------------------------------------------------- */
/* Word-buffer the stream: fwFlashWrite takes multiples of 4; keep the tail. */
static uint8_t  sPend[4];
static uint32_t sPendLen;
static uint32_t sWritten;    /* flash offset programmed so far */
static uint32_t sCrcRun;     /* running CRC32 state (pre-final-xor form) */

/* Incremental CRC32 (same polynomial/orientation as settings_crc32). */
static uint32_t crc32Feed(uint32_t crc, const uint8_t *p, uint32_t len)
{
  while (len--) {
    crc ^= *p++;
    for (int i = 0; i < 8; i++)
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return crc;
}

static void fwFail(const char *reason)
{
  sReason = reason;
  sState = FW_ERROR;
  fwFlashEnd();
  close(FW_SOCKET);
}

static void fwFinishStream(void)
{
  /* pad the tail to a word with 0xFF (erased-flash value, past the real image) */
  if (sPendLen) {
    while (sPendLen < 4U) sPend[sPendLen++] = 0xFFU;
    if (fwFlashWrite(sWritten, sPend, 4U)) { fwFail("flash"); return; }
    sWritten += 4U;
    sPendLen = 0;
  }
  fwFlashEnd();
  close(FW_SOCKET);
  if ((~sCrcRun) != sCrcWant) { sReason = "crc"; sState = FW_ERROR; return; }
  sState = FW_DONE;
}

static void fwFeed(const uint8_t *p, uint32_t n)
{
  if (sReceived + n > sSize) { fwFail("size"); return; }
  sCrcRun = crc32Feed(sCrcRun, p, n);
  sReceived += n;

  /* fill the pending word first */
  while (n && sPendLen) {
    sPend[sPendLen++] = *p++; n--;
    if (sPendLen == 4U) {
      if (fwFlashWrite(sWritten, sPend, 4U)) { fwFail("flash"); return; }
      sWritten += 4U; sPendLen = 0;
    }
  }
  uint32_t whole = n & ~3U;
  if (whole) {
    if (fwFlashWrite(sWritten, p, whole)) { fwFail("flash"); return; }
    sWritten += whole; p += whole; n -= whole;
  }
  while (n--) sPend[sPendLen++] = *p++;

  if (sReceived == sSize) fwFinishStream();
}

/* ---- data-receiver task ----------------------------------------------------- */
static const osThreadAttr_t kFwTaskAttr = {
  .name = "fwupdate", .stack_size = 512 * 4, .priority = (osPriority_t) osPriorityNormal,
};

static _Noreturn void fwTask(void *arg)
{
  (void)arg;
  static uint8_t chunk[1024];

  for (;;) {
    osThreadFlagsWait(FW_FLAG_ARMED, osFlagsWaitAny, osWaitForever);

    uint32_t lastActivity = osKernelGetTickCount();
    while (sState == FW_RECV) {
      uint8_t sr = getSn_SR(FW_SOCKET);
      if (sr == SOCK_ESTABLISHED || sr == SOCK_CLOSE_WAIT) {
        int32_t avail = (int32_t)getSn_RX_RSR(FW_SOCKET);
        if (avail > 0) {
          int32_t n = recv(FW_SOCKET, chunk,
                           (avail > (int32_t)sizeof(chunk)) ? (uint16_t)sizeof(chunk)
                                                            : (uint16_t)avail);
          if (n > 0) {
            fwFeed(chunk, (uint32_t)n);          /* may complete or fail the session */
            lastActivity = osKernelGetTickCount();
            continue;                            /* drain without the poll delay */
          }
        } else if (sr == SOCK_CLOSE_WAIT) {
          /* peer closed with bytes missing (fwFeed completes on the last byte) */
          fwFail("size");
          break;
        }
      } else if (sr == SOCK_CLOSED) {
        fwFail("size");                          /* connection lost before completion */
        break;
      }
      if ((osKernelGetTickCount() - lastActivity) > FW_TIMEOUT_MS) {
        fwFail("timeout");
        break;
      }
      osDelay(FW_POLL_MS);
    }
  }
}

/* ---- public API (called from the CLI task under the protocol mutex) -------- */
void FwUpdateStart(rampsSharedData_t *shared)
{
  sShared = shared;
  sTask = osThreadNew(fwTask, NULL, &kFwTaskAttr);
}

int FwUpdateBegin(uint8_t bank, uint32_t size, uint32_t crc)
{
  if (bank >= BANK_COUNT || size == 0U || size > APP_REGION_SIZE) return -1;
  if (sState == FW_RECV) return -2;
  if (NetGetState() != NET_STATE_LEASED) return -3;

  uint16_t port = (uint16_t)((sShared->net.port ? sShared->net.port : 5555U) + 1U);
  close(FW_SOCKET);
  if (socket(FW_SOCKET, Sn_MR_TCP, port, 0) != FW_SOCKET || listen(FW_SOCKET) != SOCK_OK) {
    close(FW_SOCKET);
    return -3;
  }

  sBank = bank; sSize = size; sCrcWant = crc;
  sReceived = 0; sWritten = 0; sPendLen = 0;
  sCrcRun = 0xFFFFFFFFU;
  sReason = "";
  fwFlashBegin(BANK_BASE(bank), BANK_SECTOR(bank));
  sState = FW_RECV;
  osThreadFlagsSet(sTask, FW_FLAG_ARMED);
  return 0;
}

void FwUpdateAbort(void)
{
  if (sState == FW_RECV) fwFail("abort");
  sState = FW_IDLE;
}

fw_state_t  FwUpdateState(void)    { return sState; }
uint32_t    FwUpdateReceived(void) { return sReceived; }
const char *FwUpdateReason(void)   { return sReason; }
uint8_t     FwUpdateBank(void)     { return sBank; }

int FwUpdateCommit(uint32_t *regionCrc)
{
  if (sState != FW_DONE) return -1;

  /* Same vector sanity the bootloader applies (banks hold Exec-linked images). */
  uint32_t base = BANK_BASE(sBank);
  uint32_t sp = *(volatile uint32_t *)base;
  uint32_t pc = *(volatile uint32_t *)(base + 4U);
  if (sp < 0x20000000U || sp > 0x20020000U) return -1;
  if (pc <  APP_EXEC_BASE || pc >= APP_REGION_END) return -1;

  uint32_t crc = settings_crc32((const void *)base, APP_REGION_SIZE);
  if (SettingsCommitBank(sBank, crc) != 0) return -2;
  if (regionCrc) *regionCrc = crc;
  sState = FW_IDLE;
  return 0;
}
