/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Bootloader CLI — see bl_cli.h. Polled USART line reader + the same response framing
 * as the app (key=value\n …, crc=HH\n, blank line; optional `*HH` request checksum).
 * Commands drive the dual-bank flash cycle: inspect/select banks, receive an image into
 * a bank (YMODEM), copy the active bank into Exec, and boot/reset.
 */
#include <string.h>
#include "stm32f4xx_hal.h"
#include "Bootloader.h"
#include "Settings.h"
#include "flash.h"
#include "ymodem.h"
#include "bl_boot.h"
#include "bl_cli.h"
#include "bl_led.h"
#include "bl_io.h"

#ifndef BL_VERSION
#define BL_VERSION "bootloader"
#endif

#define CLI_LINE_MAX 64
#define CLI_SETTLE_MS 2            /* RS485 RX->TX turnaround settle */

static UART_HandleTypeDef *s_uart;
static uint8_t  s_crc;
static uint8_t  s_reset_pending;

/* ---- low-level I/O ---- */
/* Short timeout so the idle loop can service the LED heartbeat between bytes. */
static int  rx1(uint8_t *b) { return HAL_UART_Receive(s_uart, b, 1U, 20U) == HAL_OK ? 0 : -1; }
/* bl_tx (main.c) wraps the transmit in the PA11 DE assert/release for the SP3485. */
static void tx_raw(const char *s) { bl_tx((const uint8_t *)s, (uint16_t)strlen(s)); }
static void tx_str(const char *s) { for (const char *p = s; *p; ++p) s_crc ^= (uint8_t)*p; tx_raw(s); }

/* ---- hex / checksum (XOR-8, matches the app) ---- */
static char hex_digit(uint8_t v) { return "0123456789ABCDEF"[v & 0xF]; }
static void to_hex2(uint8_t b, char *o) { o[0] = hex_digit(b >> 4); o[1] = hex_digit(b); o[2] = '\0'; }
static int  from_hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
static int     is_hex2(const char *s) { return s[0] && s[1] && !s[2] && from_hex(s[0]) >= 0 && from_hex(s[1]) >= 0; }
static uint8_t parse_hex2(const char *s) { return (uint8_t)((from_hex(s[0]) << 4) | from_hex(s[1])); }
static uint8_t cksum(const char *s) { uint8_t c = 0; while (*s) c ^= (uint8_t)*s++; return c; }

/* ---- response framing ---- */
static void resp_begin(void) { HAL_Delay(CLI_SETTLE_MS); s_crc = 0; }
static void resp_kv(const char *k, const char *v) { tx_str(k); tx_str("="); tx_str(v); tx_str("\n"); }
static void resp_err(const char *r) { tx_str("error="); tx_str(r); tx_str("\n"); }
static void resp_end(void) { char h[3]; to_hex2(s_crc, h); tx_raw("crc="); tx_raw(h); tx_raw("\n\n"); }

static void utoa10(uint32_t v, char *out) {       /* unsigned -> decimal string */
  char tmp[11]; int i = 0;
  do { tmp[i++] = (char)('0' + v % 10U); v /= 10U; } while (v);
  int j = 0; while (i) out[j++] = tmp[--i]; out[j] = '\0';
}
static void resp_kv_u(const char *k, uint32_t v) { char b[12]; utoa10(v, b); resp_kv(k, b); }
static void resp_kv_hex(const char *k, uint32_t v) {
  char b[9]; for (int i = 0; i < 8; i++) b[i] = hex_digit((uint8_t)(v >> (28 - 4 * i)));
  b[8] = '\0'; resp_kv(k, b);
}

/* ---- helpers ---- */
static int parse_bank(const char *s, uint8_t *out) {
  if (s[0] == '0' && !s[1]) { *out = 0; return 0; }
  if (s[0] == '1' && !s[1]) { *out = 1; return 0; }
  return -1;
}

/* ---- commands ---- */
static void cmd_version(int argc, char **argv) { (void)argc; (void)argv; resp_kv("version", BL_VERSION); }

static void cmd_info(int argc, char **argv) {
  (void)argc; (void)argv;
  settings_t s; settings_load(&s);
  resp_kv_u("boot.mode",   s.boot_mode);
  resp_kv_u("bank.active", s.active_bank);
  resp_kv_u("bank.loaded", s.loaded_bank);
  resp_kv_u("bank0.valid", (uint32_t)bl_image_valid(BANK_BASE(0)));
  resp_kv_u("bank1.valid", (uint32_t)bl_image_valid(BANK_BASE(1)));
  resp_kv_u("exec.valid",  (uint32_t)bl_image_valid(APP_EXEC_BASE));
}

static void cmd_bank(int argc, char **argv) {     /* `bank` info | `bank <n>` select+persist */
  if (argc < 2) { cmd_info(argc, argv); return; }
  uint8_t n;
  if (parse_bank(argv[1], &n)) { resp_err("usage: bank <0|1>"); return; }
  settings_t s; settings_load(&s);
  s.active_bank = n;
  if (flash_write_settings(&s)) { resp_err("settings write"); return; }
  resp_kv_u("bank.active", n);
}

static void cmd_boot_mode(int argc, char **argv) { /* persist boot mode (app|bl) */
  if (argc < 2) { resp_err("usage: boot.mode <app|bl>"); return; }
  uint16_t m;
  if      (!strcmp(argv[1], "app")) m = BOOT_MODE_APP;
  else if (!strcmp(argv[1], "bl"))  m = BOOT_MODE_BOOTLOADER;
  else { resp_err("usage: boot.mode <app|bl>"); return; }
  settings_t s; settings_load(&s);
  s.boot_mode = m;
  if (flash_write_settings(&s)) { resp_err("settings write"); return; }
  resp_kv_u("boot.mode", m);
}

static void cmd_erase(int argc, char **argv) {
  uint8_t n;
  if (argc < 2 || parse_bank(argv[1], &n)) { resp_err("usage: erase <0|1>"); return; }
  if (flash_erase_sector(BANK_SECTOR(n))) { resp_err("erase"); return; }
  resp_kv_u("erase", n);
}

static void cmd_crc(int argc, char **argv) {       /* CRC32 over the 128 KB bank region */
  uint8_t n;
  if (argc < 2 || parse_bank(argv[1], &n)) { resp_err("usage: crc <0|1>"); return; }
  uint32_t c = settings_crc32((const void *)BANK_BASE(n), APP_REGION_SIZE);
  resp_kv_hex("crc", c);
}

static void cmd_flash(int argc, char **argv) {     /* YMODEM-receive into a bank */
  uint8_t n;
  if (argc < 2 || parse_bank(argv[1], &n)) { resp_err("usage: flash <0|1>"); return; }
  /* No framed "ready": the client starts YMODEM on our 'C'. We frame the result after. */
  uint32_t size = 0;
  int rc = ymodem_receive(s_uart, BANK_BASE(n), BANK_SECTOR(n), &size);
  if (rc != YM_OK)                  { resp_err("ymodem"); return; }
  if (!bl_image_valid(BANK_BASE(n))){ resp_err("bad image"); return; }
  /* Record the bank's region CRC32 so boot can detect later corruption. */
  settings_t s; settings_load(&s);
  s.bank_crc[n] = settings_crc32((const void *)BANK_BASE(n), APP_REGION_SIZE);
  flash_write_settings(&s);
  resp_kv_u("flash", n);
  resp_kv_u("size", size);
  resp_kv_hex("crc", s.bank_crc[n]);
}

static void cmd_copy(int argc, char **argv) {      /* force active bank -> Exec */
  (void)argc; (void)argv;
  settings_t s; settings_load(&s);
  uint8_t bank = (s.active_bank < BANK_COUNT) ? s.active_bank : 0U;
  if (!bl_bank_trusted(bank, &s)) { resp_err("bad image"); return; }
  if (bl_load_bank(bank))         { resp_err("copy"); return; }
  s.loaded_bank = bank;
  flash_write_settings(&s);
  resp_kv_u("copy", bank);
}

static void cmd_boot(int argc, char **argv) {      /* run the app (copy if stale, then jump) */
  (void)argc; (void)argv;
  if (bl_boot_app() != 0) resp_err("no app");      /* only returns on failure */
}

static void cmd_rollback(int argc, char **argv) {  /* switch to the other bank and boot it */
  (void)argc; (void)argv;
  settings_t s; settings_load(&s);
  uint8_t other = s.active_bank ? 0u : 1u;
  if (!bl_bank_trusted(other, &s)) { resp_err("other bank invalid"); return; }
  s.active_bank = other;
  if (flash_write_settings(&s))    { resp_err("settings write"); return; }
  bl_boot_app();                                   /* copies other->Exec, jumps; returns on failure */
  resp_err("boot failed");
}

static void cmd_reset(int argc, char **argv) {     /* full system reset (re-samples BOOT0) */
  (void)argc; (void)argv;
  resp_kv("reset", "ok");
  s_reset_pending = 1;                             /* run loop resets after the ack drains */
}

typedef void (*cli_fn)(int, char **);
typedef struct { const char *name; cli_fn fn; const char *help; } cli_cmd_t;
static const cli_cmd_t kCommands[] = {
  { "version",   cmd_version,   "bootloader version" },
  { "info",      cmd_info,      "banks: mode/active/loaded/validity" },
  { "bank",      cmd_bank,      "bank [<0|1>] — info, or select active (persist)" },
  { "boot.mode", cmd_boot_mode, "boot.mode <app|bl> — persistent boot target" },
  { "flash",     cmd_flash,     "flash <0|1> — receive image into a bank (YMODEM)" },
  { "erase",     cmd_erase,     "erase <0|1>" },
  { "crc",       cmd_crc,       "crc <0|1> — CRC32 of a bank region" },
  { "copy",      cmd_copy,      "copy active bank into Exec" },
  { "rollback",  cmd_rollback,  "switch to the other bank and boot it" },
  { "boot",      cmd_boot,      "run the app (jump; no reset)" },
  { "reset",     cmd_reset,     "system reset" },
  { NULL, NULL, NULL },
};

static void cmd_help(int argc, char **argv) {
  (void)argc; (void)argv;
  for (const cli_cmd_t *c = kCommands; c->name; c++) resp_kv(c->name, c->help);
}

static void process_line(char *line) {
  resp_begin();
  /* optional `*HH` checksum: validate then strip */
  char *star = strrchr(line, '*');
  if (star) {
    if (!is_hex2(star + 1)) { resp_err("bad checksum"); resp_end(); return; }
    uint8_t want = parse_hex2(star + 1);
    *star = '\0';
    if (cksum(line) != want) { resp_err("bad checksum"); resp_end(); return; }
  }
  char *argv[4]; int argc = 0;
  for (char *t = strtok(line, " "); t && argc < 4; t = strtok(NULL, " ")) argv[argc++] = t;
  if (argc == 0) { resp_err("empty command"); resp_end(); return; }

  if (!strcmp(argv[0], "help")) { cmd_help(argc, argv); resp_end(); return; }
  for (const cli_cmd_t *c = kCommands; c->name; c++)
    if (!strcmp(argv[0], c->name)) { c->fn(argc, argv); resp_end(); return; }
  resp_err("unknown command");
  resp_end();
}

void bl_cli_run(UART_HandleTypeDef *huart) {
  s_uart = huart;
  s_reset_pending = 0;
  resp_begin(); resp_kv("bootloader", "ready"); resp_end();   /* greeting => CLI mode */

  char line[CLI_LINE_MAX + 1];
  uint16_t len = 0;
  for (;;) {
    uint8_t b;
    if (rx1(&b)) { bl_led_service(); continue; }   /* idle: render the heartbeat */
    if (b == '\r' || b == '\n') {
      if (len == 0) continue;                  /* ignore blank input lines */
      line[len] = '\0';
      len = 0;
      process_line(line);
      if (s_reset_pending) { HAL_Delay(20); NVIC_SystemReset(); }
    } else if (len < CLI_LINE_MAX) {
      line[len++] = (char)b;
    }
  }
}
