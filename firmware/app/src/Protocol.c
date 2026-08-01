/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * drDRO line protocol. Command in (text, \r/\n/both terminated) → key=value
 * lines out, terminated by an empty line; errors carry `error=<reason>`.
 * Ported from the drdro-firmware-f4 baseline; wire format unchanged.
 *
 * V1.5 changes vs the baseline:
 *   - transport-independent core: every response goes through a proto_io_t
 *     (Protocol.h). This file owns the RS-485 UART instance; NetCli.c owns the
 *     TCP one. A mutex serializes dispatch across transports.
 *   - TX uses Rs485Send() (usart.c): DE asserted around an interrupt-driven
 *     transmit, released on TC — the baseline board had an auto-direction
 *     transceiver and used bare blocking HAL TX.
 *   - registry arrays are 5-wide (SCALES_COUNT) and add `scales.dir`.
 *
 * ProtocolStart() owns USART1: byte-IT RX feeds ProtocolFeedByte() (ISR), which
 * wakes a FreeRTOS service task on a complete line; the task parses, dispatches,
 * and writes the response.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include "cmsis_os2.h"
#include "Protocol.h"
#include "Bootloader.h"
#include "SettingsStore.h"
#include "usart.h"
#include "Aout.h"
#include "FwUpdate.h"

/* ---- bound UART + shared data ------------------------------------------- */
static UART_HandleTypeDef *sUart = NULL;
static rampsSharedData_t  *sShared = NULL;

/* Serializes parse+dispatch across transports (RS-485 task vs TCP task) so
 * flash writes / live-apply hooks never interleave. Registry field reads stay
 * lock-free (aligned 16/32-bit stores are atomic vs the TIM9 ISR). */
static osMutexId_t sProtoMutex = NULL;

/* ---- RX task + activity ------------------------------------------------- */
static volatile uint32_t sRxLines  = 0;   /* processed-command counter           */
static osThreadId_t      sTask = NULL;
static const osThreadAttr_t kProtoTaskAttr = {
  .name = "protocol", .stack_size = 512 * 4, .priority = (osPriority_t) osPriorityNormal,
};
static void protocolTask(void *arg);

/* ---- RX line assembly (CRLF-safe; preserves the deliberate empty-line repeat) */
static char    sLine[PROTOCOL_LINE_MAX + 1];   /* in-progress line                */
static uint16_t sLen   = 0;
static char    sPendEOL = 0;                   /* last terminator, for \r\n pairs */
static char    sReady[PROTOCOL_LINE_MAX + 1];  /* last completed line             */
static volatile uint8_t sReadyFlag = 0;

/* ---- checksum (XOR-8, hex) — isolated so it can become CRC-8/16 later ---- */
static uint8_t protoCksum(const char *buf, int len) {
  uint8_t c = 0;
  for (int i = 0; i < len; i++) c ^= (uint8_t)buf[i];
  return c;
}
static char hexDigit(uint8_t v) { return "0123456789ABCDEF"[v & 0xF]; }
static void toHex2(uint8_t b, char *out) { out[0] = hexDigit(b >> 4); out[1] = hexDigit(b); out[2] = '\0'; }
static int  fromHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
static int     isHex2(const char *s) { return s[0] && s[1] && !s[2] && fromHex(s[0]) >= 0 && fromHex(s[1]) >= 0; }
static uint8_t parseHex2(const char *s) { return (uint8_t)((fromHex(s[0]) << 4) | fromHex(s[1])); }

/* ---- response helpers (all routed through the transport's proto_io_t) ---- */
static void txRaw(proto_io_t *io, const char *s) {   /* send, not counted toward crc */
  io->writeFn(io, s, (uint16_t)strlen(s));
}
static void txStr(proto_io_t *io, const char *s) {   /* send + accumulate crc        */
  for (const char *p = s; *p; ++p) io->crc ^= (uint8_t)*p;
  txRaw(io, s);
}
static void respBegin(proto_io_t *io) {
  if (io->beginFn) io->beginFn(io);
  io->crc = 0;
}
static void respKV(proto_io_t *io, const char *key, const char *val) {
  txStr(io, key); txStr(io, "="); txStr(io, val); txStr(io, "\n");
}
static void respError(proto_io_t *io, const char *reason) {
  txStr(io, "error="); txStr(io, reason); txStr(io, "\n");
}
static void respEnd(proto_io_t *io) {   /* crc=HH then the terminating empty line */
  char h[3]; toHex2(io->crc, h);
  txRaw(io, "crc="); txRaw(io, h); txRaw(io, "\n");
  txRaw(io, "\n");
  if (io->endFn) io->endFn(io);
}

/* ---- the RS-485 UART transport ------------------------------------------- */
/* RS485 is half-duplex: after we finish receiving the command we let the line
 * settle before driving TX, or the opening bytes of the response are swallowed
 * by the RX->TX turnaround. Runs in protocolTask context, so osDelay is safe.
 * Kept from the baseline for host-timing compatibility (the DE pin actually
 * makes the turnaround explicit on this board). */
#define PROTOCOL_TX_SETTLE_MS 2   /* verified clean on the baseline bench */
/* Responses are accumulated and sent as ONE gap-free burst (single DE assert):
 * sending fragment-by-fragment put ~30 DE toggles + inter-byte gaps in every
 * response, and at 1 Mbaud the byte at each boundary was occasionally lost
 * (seen as CRC-bad frames on the bench). Buffer sized for the largest response
 * (`help` ~700 B); anything bigger flushes mid-response, which just costs one
 * extra turnaround. */
static uint8_t  sUartTxBuf[768];
static uint16_t sUartTxLen = 0;
static void uartFlush(void) {
  if (sUartTxLen) { Rs485Send(sUartTxBuf, sUartTxLen); sUartTxLen = 0; }
}
static void uartIoWrite(proto_io_t *io, const char *s, uint16_t n) {
  (void)io;
  while (n) {
    uint16_t room = (uint16_t)(sizeof(sUartTxBuf) - sUartTxLen);
    if (room == 0U) { uartFlush(); continue; }
    uint16_t chunk = (n < room) ? n : room;
    memcpy(sUartTxBuf + sUartTxLen, s, chunk);
    sUartTxLen += chunk;
    s += chunk;
    n -= chunk;
  }
}
static void uartIoBegin(proto_io_t *io) {
  (void)io;
  sUartTxLen = 0;
  osDelay(PROTOCOL_TX_SETTLE_MS);
}
static void uartIoEnd(proto_io_t *io) {
  (void)io;
  uartFlush();
}
static proto_io_t sUartIo = { uartIoWrite, uartIoBegin, uartIoEnd, NULL, 0, {0} };

/* ---- command handlers ---------------------------------------------------- */
typedef void (*cmd_fn)(proto_io_t *io, int argc, char **argv);
typedef struct { const char *name; cmd_fn fn; const char *help; } cmd_t;
static const cmd_t kCommands[];   /* fwd decl */

/* Post-response action requested by the dispatched command (per call). */
static proto_action_t sAction = PROTO_ACT_NONE;

static void cmdVersion(proto_io_t *io, int argc, char **argv) {
  (void)argc; (void)argv;
  respKV(io, "version", FW_VERSION);
}

static void cmdHelp(proto_io_t *io, int argc, char **argv) {
  (void)argc; (void)argv;
  for (const cmd_t *c = kCommands; c->name; c++) respKV(io, c->name, c->help);
}

/* ---- variable registry (array-aware) ------------------------------------ */
typedef enum { VT_I32, VT_U32, VT_F32, VT_U16, VT_IP4, VT_MAC, VT_UPT } var_type_t;
#define V_RO 1
typedef struct {
  const char *name;
  size_t      offset;   /* byte offset of element [0] within rampsSharedData_t */
  var_type_t  type;
  uint8_t     count;    /* 1 = scalar, N = array */
  uint16_t    stride;   /* bytes between array elements */
  uint8_t     flags;    /* V_RO */
  uint32_t    umax;     /* write bound for unsigned types (0 = no bound) */
} var_entry_t;

#define OFF(f) offsetof(rampsSharedData_t, f)
static const var_entry_t kVars[] = {
  { "scales.count",  OFF(scaleCount),                 VT_U16, 1, 0, V_RO },
  { "scales.pos",    OFF(scales[0].position),         VT_I32, SCALES_COUNT, sizeof(input_t), 0    },
  { "scales.speed",  OFF(scales[0].speed),            VT_I32, SCALES_COUNT, sizeof(input_t), V_RO },
  { "scales.num",    OFF(scales[0].syncRatioNum),     VT_I32, SCALES_COUNT, sizeof(input_t), 0    },
  { "scales.den",    OFF(scales[0].syncRatioDen),     VT_I32, SCALES_COUNT, sizeof(input_t), 0    },
  { "scales.sync",   OFF(scales[0].syncEnable),       VT_U16, SCALES_COUNT, sizeof(input_t), 0    },
  { "scales.filt",   OFF(scales[0].filterValue),      VT_U16, SCALES_COUNT, sizeof(input_t), 0, SCALES_FILTER_MAX },
  { "scales.dir",    OFF(scales[0].dirInvert),        VT_U16, SCALES_COUNT, sizeof(input_t), 0, 1 },
  { "servo.max",     OFF(servo.maxSpeed),             VT_F32, 1, 0, 0    },
  { "servo.acc",     OFF(servo.acceleration),         VT_F32, 1, 0, 0    },
  { "servo.jog",     OFF(servo.jogSpeed),             VT_F32, 1, 0, 0    },
  { "servo.idx",     OFF(servo.indexSpeed),           VT_F32, 1, 0, 0    },
  { "servo.mode",    OFF(fastData.servoMode),         VT_U16, 1, 0, 0    },
  { "servo.pos",     OFF(servo.currentSteps),         VT_U32, 1, 0, V_RO },
  { "servo.speed",   OFF(servo.currentSpeed),         VT_F32, 1, 0, V_RO },
  { "servo.tgt",     OFF(servo.stepsToGo),            VT_I32, 1, 0, 0    },
  { "din.state",     OFF(din.state),                  VT_U16, 1, 0, V_RO },
  { "din.cnt",       OFF(din.count[0]),               VT_U32, 6, sizeof(uint32_t), V_RO },
  { "din.deb",       OFF(din.debounceMs),             VT_U16, 1, 0, 0, 250 },
  { "aout.raw",      OFF(aout.raw[0]),                VT_U16, 2, sizeof(uint16_t), 0, 4095 },
  /* live network status (RO) + persisted config (applied at the next boot) */
  { "net.state",     OFF(net.state),                  VT_U16, 1, 0, V_RO },
  { "net.mac",       OFF(net.mac[0]),                 VT_MAC, 1, 0, V_RO },
  { "net.addr",      OFF(net.ip[0]),                  VT_IP4, 1, 0, V_RO },
  { "net.mask",      OFF(net.mask[0]),                VT_IP4, 1, 0, V_RO },
  { "net.gw",        OFF(net.gw[0]),                  VT_IP4, 1, 0, V_RO },
  { "net.dhcp",      OFF(net.dhcp),                   VT_U16, 1, 0, 0, 1 },
  { "net.port",      OFF(net.port),                   VT_U16, 1, 0, 0, 65535 },
  { "net.cfg.ip",    OFF(net.cfgIp[0]),               VT_IP4, 1, 0, 0    },
  { "net.cfg.mask",  OFF(net.cfgMask[0]),             VT_IP4, 1, 0, 0    },
  { "net.cfg.gw",    OFF(net.cfgGw[0]),               VT_IP4, 1, 0, 0    },
  { "com.baud",      OFF(comBaud),                    VT_U32, 1, 0, 0, 2000000 },  /* next boot; BL stays 115200 */
  { "diag.cycles",   OFF(fastData.cycles),            VT_U32, 1, 0, V_RO },
  { "diag.interval", OFF(fastData.executionInterval), VT_U32, 1, 0, V_RO },
  { "diag.cycmax",   OFF(diag.cyclesMax),             VT_U32, 1, 0, 0    },  /* write 0 to re-arm */
  { "diag.uptime",   OFF(diag.uptimeS),               VT_UPT, 1, 0, V_RO },
  { NULL, 0, 0, 0, 0, 0, 0 },
};

static const var_entry_t *findVar(const char *name) {
  for (const var_entry_t *v = kVars; v->name; v++)
    if (strcmp(name, v->name) == 0) return v;
  return NULL;
}
static void *fieldPtr(const var_entry_t *v, int idx) {
  return (uint8_t *)sShared + v->offset + (size_t)idx * v->stride;
}
static void formatField(const var_entry_t *v, int idx, char *out, size_t n) {
  void *p = fieldPtr(v, idx);
  switch (v->type) {
    case VT_I32: snprintf(out, n, "%ld", (long)*(int32_t *)p); break;
    case VT_U32: snprintf(out, n, "%lu", (unsigned long)*(uint32_t *)p); break;
    case VT_U16: snprintf(out, n, "%u", (unsigned)*(uint16_t *)p); break;
    case VT_F32: snprintf(out, n, "%g", (double)*(float *)p); break;
    case VT_IP4: {
      const uint8_t *b = (const uint8_t *)p;
      snprintf(out, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
      break;
    }
    case VT_MAC: {
      const uint8_t *b = (const uint8_t *)p;
      snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
      break;
    }
    case VT_UPT: {   /* u32 seconds -> "<days>d<HH>:<MM>:<SS>" */
      uint32_t t = *(uint32_t *)p;
      snprintf(out, n, "%lud%02lu:%02lu:%02lu",
               (unsigned long)(t / 86400U), (unsigned long)(t / 3600U % 24U),
               (unsigned long)(t / 60U % 60U), (unsigned long)(t % 60U));
      break;
    }
  }
}
/* "a.b.c.d" with each octet 0..255, no trailing junk. Returns 0 ok, -1 error. */
static int parseIp4(const char *s, uint8_t out[4]) {
  for (int i = 0; i < 4; i++) {
    if (*s < '0' || *s > '9') return -1;
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9') {
      v = v * 10U + (unsigned long)(*s++ - '0');
      if (v > 255U) return -1;
    }
    if (i < 3) { if (*s++ != '.') return -1; }
    out[i] = (uint8_t)v;
  }
  return (*s == '\0') ? 0 : -1;
}
/* parse+store; returns 0 ok, -1 on parse/range error. 32-bit/16-bit aligned
 * stores are atomic on M4 vs the TIM9 ISR, so no lock is needed. */
static int writeField(const var_entry_t *v, int idx, const char *s) {
  void *p = fieldPtr(v, idx);
  char *end = NULL;
  switch (v->type) {
    case VT_I32: { long  x = strtol(s, &end, 0);  if (*end) return -1; *(int32_t  *)p = (int32_t)x;  break; }
    case VT_U32: { unsigned long x = strtoul(s, &end, 0); if (*end || (v->umax && x > v->umax)) return -1; *(uint32_t *)p = (uint32_t)x; break; }
    case VT_U16: { unsigned long x = strtoul(s, &end, 0); if (*end || x > 0xFFFF || (v->umax && x > v->umax)) return -1; *(uint16_t *)p = (uint16_t)x; break; }
    case VT_F32: { float x = strtof(s, &end);     if (*end) return -1; *(float    *)p = x;           break; }
    case VT_IP4: { uint8_t ip[4]; if (parseIp4(s, ip)) return -1; memcpy(p, ip, 4); break; }
    case VT_MAC: return -1;   /* read-only by nature (derived from the MCU UID) */
    case VT_UPT: return -1;   /* read-only by nature */
  }
  return 0;
}
static void emitVar(proto_io_t *io, const var_entry_t *v) {
  char val[24];
  txStr(io, v->name); txStr(io, "=");
  for (int i = 0; i < v->count; i++) {
    if (i) txStr(io, ",");
    formatField(v, i, val, sizeof(val));
    txStr(io, val);
  }
  txStr(io, "\n");
}

/* ---- registry-backed commands ------------------------------------------- */
static void cmdGet(proto_io_t *io, int argc, char **argv) {
  if (argc < 2) { respError(io, "usage: get <name>"); return; }
  const var_entry_t *v = findVar(argv[1]);
  if (!v) { respError(io, "unknown variable"); return; }
  emitVar(io, v);
}
static void cmdSettings(proto_io_t *io, int argc, char **argv) {
  (void)argc; (void)argv;
  for (const var_entry_t *v = kVars; v->name; v++) emitVar(io, v);
}
static void cmdSet(proto_io_t *io, int argc, char **argv) {
  if (argc < 2) { respError(io, "usage: set <name> [idx] <value>"); return; }
  const var_entry_t *v = findVar(argv[1]);
  if (!v) { respError(io, "unknown variable"); return; }
  if (v->flags & V_RO) { respError(io, "read-only"); return; }

  int idx = 0;
  const char *valStr;
  if (v->count > 1) {                        /* array: need <idx> <value> */
    if (argc < 4) { respError(io, "usage: set <name> <idx> <value>"); return; }
    char *end = NULL;
    long i = strtol(argv[2], &end, 10);
    if (*end || i < 0 || i >= v->count) { respError(io, "bad index"); return; }
    idx = (int)i; valStr = argv[3];
  } else {                                    /* scalar: <value> */
    if (argc < 3) { respError(io, "usage: set <name> <value>"); return; }
    valStr = argv[2];
  }
  if (writeField(v, idx, valStr) != 0) { respError(io, "value out of range"); return; }
  /* live-apply hooks: some variables are hardware state, not just memory */
  if (strcmp(v->name, "scales.filt") == 0)
    setScaleFilter(sShared->scales[idx].timerHandle, sShared->scales[idx].filterValue);
  else if (strcmp(v->name, "aout.raw") == 0) {
    if (AoutWrite((uint8_t)idx, sShared->aout.raw[idx]) != 0) { respError(io, "i2c"); return; }
  }
  /* success: no body line; respEnd() emits crc + empty line */
}
static void cmdSta(proto_io_t *io, int argc, char **argv) {
  (void)argc; (void)argv;
  static const char *kStaVars[] = {
    "scales.pos", "scales.speed", "servo.pos", "servo.speed", "servo.tgt", "servo.mode",
    "din.state"
  };
  for (unsigned i = 0; i < sizeof(kStaVars) / sizeof(kStaVars[0]); i++) {
    const var_entry_t *v = findVar(kStaVars[i]);
    if (v) emitVar(io, v);
  }
}

/* Enter the IAP bootloader: arm the handshake word, ack, then hand off (done by
 * the transport once the ack has fully left the wire). See shared/Bootloader.h. */
static void cmdUpdate(proto_io_t *io, int argc, char **argv) {
  (void)argc; (void)argv;
  BOOT_FLAG = BOOT_MAGIC;
  sAction = PROTO_ACT_HANDOFF;
  respKV(io, "update", "ready");
}

/* Restart without the bootloader request — lets the client drive the boot cycle.
 * Handled as a handoff to 0x08000000: with no BOOT_FLAG and boot_mode=app the
 * bootloader immediately (re)boots the app. */
static void cmdReset(proto_io_t *io, int argc, char **argv) {
  (void)argc; (void)argv;
  sAction = PROTO_ACT_HANDOFF;
  respKV(io, "reset", "ok");
}

static void cmdSave(proto_io_t *io, int argc, char **argv) {
  (void)argc; (void)argv;
  if (SettingsSave(sShared) != 0)    { respError(io, "flash write"); return; }
  respKV(io, "save", "ok");
}
static void cmdLoad(proto_io_t *io, int argc, char **argv) {
  (void)argc; (void)argv;
  int ok = SettingsLoad(sShared);
  for (int i = 0; i < SCALES_COUNT; i++)   /* reloaded filters are hardware state — reapply */
    setScaleFilter(sShared->scales[i].timerHandle, sShared->scales[i].filterValue);
  respKV(io, "load", ok ? "ok" : "default");
}
static void cmdBank(proto_io_t *io, int argc, char **argv) {
  char buf[4];
  if (argc < 2) {                    /* report the persisted active bank */
    snprintf(buf, sizeof(buf), "%u", (unsigned)SettingsActiveBank());
    respKV(io, "bank.active", buf);
    return;
  }
  if ((argv[1][0] != '0' && argv[1][0] != '1') || argv[1][1]) { respError(io, "usage: bank <0|1>"); return; }
  if (SettingsBankSet((uint8_t)(argv[1][0] - '0')) != 0) { respError(io, "flash write"); return; }
  respKV(io, "bank.active", argv[1]);
}

/* Raw digital-output override for bring-up: drive the ULN2003 channels that are
 * not currently claimed by the motion core. ENA and M1 belong to motion while
 * servo.mode != 0; M2/M3 are always free (no motion support yet). Remember the
 * ULN2003 inverts — verify levels at the connector. */
typedef struct { const char *name; GPIO_TypeDef *port; uint16_t pin; uint8_t motionOwned; } dout_pin_t;
static const dout_pin_t kDoutPins[] = {
  { "ena",     MOTOR_GPIO_Port, M_ENA_Pin,   1 },
  { "m1.step", MOTOR_GPIO_Port, M1_STEP_Pin, 1 },
  { "m1.dir",  MOTOR_GPIO_Port, M1_DIR_Pin,  1 },
  { "m2.step", MOTOR_GPIO_Port, M2_STEP_Pin, 0 },
  { "m2.dir",  MOTOR_GPIO_Port, M2_DIR_Pin,  0 },
  { "m3.step", MOTOR_GPIO_Port, M3_STEP_Pin, 0 },
  { "m3.dir",  MOTOR_GPIO_Port, M3_DIR_Pin,  0 },
  { NULL, NULL, 0, 0 },
};
static void cmdDout(proto_io_t *io, int argc, char **argv) {
  if (argc < 3 || (argv[2][1] != '\0') || (argv[2][0] != '0' && argv[2][0] != '1')) {
    respError(io, "usage: dout <name> <0|1>"); return;
  }
  const dout_pin_t *p;
  for (p = kDoutPins; p->name; p++) if (strcmp(argv[1], p->name) == 0) break;
  if (!p->name) { respError(io, "unknown output"); return; }
  if (p->motionOwned && sShared->fastData.servoMode != 0) { respError(io, "motion active"); return; }
  HAL_GPIO_WritePin(p->port, p->pin, (argv[2][0] == '1') ? GPIO_PIN_SET : GPIO_PIN_RESET);
  char key[16];
  snprintf(key, sizeof(key), "dout.%s", p->name);
  respKV(io, key, argv[2]);
}

/* Switch to the other bank and hand off to the bootloader (which copies it into
 * Exec and runs it). Convenience over `bank <other>` + `reset`. */
static void cmdRollback(proto_io_t *io, int argc, char **argv) {
  (void)argc; (void)argv;
  uint8_t other = SettingsActiveBank() ? 0u : 1u;
  if (SettingsBankSet(other) != 0) { respError(io, "flash write"); return; }
  char b[2] = { (char)('0' + other), '\0' };
  respKV(io, "rollback", b);
  sAction = PROTO_ACT_HANDOFF;
}

/* ---- firmware update over the network (FwUpdate.h) ----------------------- */
static void respKVu(proto_io_t *io, const char *key, uint32_t v) {
  char b[12]; snprintf(b, sizeof(b), "%lu", (unsigned long)v); respKV(io, key, b);
}
static void respKVhex8(proto_io_t *io, const char *key, uint32_t v) {
  char b[9]; snprintf(b, sizeof(b), "%08lX", (unsigned long)v); respKV(io, key, b);
}

static void cmdFwBegin(proto_io_t *io, int argc, char **argv) {
  if (argc < 4) { respError(io, "usage: fw.begin <bank> <size> <crc32hex8>"); return; }
  if ((argv[1][0] != '0' && argv[1][0] != '1') || argv[1][1]) { respError(io, "usage: fw.begin <bank> <size> <crc32hex8>"); return; }
  char *end = NULL;
  unsigned long size = strtoul(argv[2], &end, 10);
  if (*end || size == 0) { respError(io, "usage: fw.begin <bank> <size> <crc32hex8>"); return; }
  unsigned long crc = strtoul(argv[3], &end, 16);
  if (*end || strlen(argv[3]) != 8) { respError(io, "usage: fw.begin <bank> <size> <crc32hex8>"); return; }
  int rc = FwUpdateBegin((uint8_t)(argv[1][0] - '0'), (uint32_t)size, (uint32_t)crc);
  if (rc == -1) { respError(io, "bad args"); return; }
  if (rc == -2) { respError(io, "busy"); return; }
  if (rc != 0)  { respError(io, "network down"); return; }
  respKV(io, "fw.state", "recv");
  respKVu(io, "fw.port", (uint32_t)((sShared->net.port ? sShared->net.port : 5555U) + 1U));
}

static void cmdFwStatus(proto_io_t *io, int argc, char **argv) {
  (void)argc; (void)argv;
  static const char *kNames[] = { "idle", "recv", "done", "error" };
  fw_state_t st = FwUpdateState();
  respKV(io, "fw.state", kNames[st]);
  respKVu(io, "fw.recv", FwUpdateReceived());
  if (st == FW_ERROR) respKV(io, "fw.reason", FwUpdateReason());
}

static void cmdFwCommit(proto_io_t *io, int argc, char **argv) {
  (void)argc; (void)argv;
  uint8_t bank = FwUpdateBank();
  uint32_t regionCrc = 0;
  int rc = FwUpdateCommit(&regionCrc);
  if (rc == -1) { respError(io, "not ready"); return; }
  if (rc != 0)  { respError(io, "flash write"); return; }
  respKV(io, "fw.commit", "ok");
  respKVu(io, "fw.bank", bank);
  respKVhex8(io, "fw.crc", regionCrc);
}

static void cmdFwAbort(proto_io_t *io, int argc, char **argv) {
  (void)argc; (void)argv;
  FwUpdateAbort();
  respKV(io, "fw.state", "idle");
}

static const cmd_t kCommands[] = {
  { "sta",      cmdSta,      "fast read: scale pos/speed + servo pos/speed/tgt/mode" },
  { "set",      cmdSet,      "set <name> [idx] <value>" },
  { "get",      cmdGet,      "get <name>" },
  { "settings", cmdSettings, "dump all variables" },
  { "save",     cmdSave,     "persist settings to flash" },
  { "load",     cmdLoad,     "reload settings from flash" },
  { "bank",     cmdBank,     "bank [<0|1>] — active bank: report, or select for next boot" },
  { "rollback", cmdRollback, "switch to the other bank and reboot into it" },
  { "dout",     cmdDout,     "dout <ena|mN.step|mN.dir> <0|1> — raw output (bring-up)" },
  { "fw.begin", cmdFwBegin,  "fw.begin <bank> <size> <crc32hex8> — arm network update" },
  { "fw.status",cmdFwStatus, "network update state" },
  { "fw.commit",cmdFwCommit, "record + activate the received bank (then `reset`)" },
  { "fw.abort", cmdFwAbort,  "abort a network update" },
  { "version",  cmdVersion,  "firmware version" },
  { "help",     cmdHelp,     "list commands" },
  { "update",   cmdUpdate,   "reboot into the firmware-update bootloader" },
  { "reset",    cmdReset,    "system reset" },
  { NULL, NULL, NULL },
};

/* ---- line processing ----------------------------------------------------- */
proto_action_t ProtocolProcessLine(proto_io_t *io, char *line) {
  char work[PROTOCOL_LINE_MAX + 1];

  if (sProtoMutex) osMutexAcquire(sProtoMutex, osWaitForever);
  sAction = PROTO_ACT_NONE;
  respBegin(io);

  if (line[0] == '\0') {                      /* empty line → repeat last command */
    if (io->last[0] == '\0') {
      respError(io, "no previous command"); respEnd(io);
      if (sProtoMutex) osMutexRelease(sProtoMutex);
      return PROTO_ACT_NONE;
    }
    strncpy(work, io->last, sizeof(work) - 1); work[sizeof(work) - 1] = '\0';
  } else {
    strncpy(work, line, sizeof(work) - 1); work[sizeof(work) - 1] = '\0';

    /* Optional `*HH` checksum: validate if present, then strip it. */
    char *star = strrchr(work, '*');
    if (star) {
      if (!isHex2(star + 1)) {
        respError(io, "bad checksum"); respEnd(io);
        if (sProtoMutex) osMutexRelease(sProtoMutex);
        return PROTO_ACT_NONE;
      }
      uint8_t want = parseHex2(star + 1);
      *star = '\0';
      if (protoCksum(work, (int)strlen(work)) != want) {
        respError(io, "bad checksum"); respEnd(io);
        if (sProtoMutex) osMutexRelease(sProtoMutex);
        return PROTO_ACT_NONE;
      }
    }
    strncpy(io->last, work, sizeof(io->last) - 1); io->last[sizeof(io->last) - 1] = '\0';  /* stripped body */
  }

  char *argv[8];
  int   argc = 0;
  for (char *tok = strtok(work, " "); tok && argc < 8; tok = strtok(NULL, " ")) argv[argc++] = tok;

  proto_action_t action = PROTO_ACT_NONE;
  if (argc == 0) {
    respError(io, "empty command"); respEnd(io);
  } else {
    const cmd_t *c;
    for (c = kCommands; c->name; c++) {
      if (strcmp(argv[0], c->name) == 0) { c->fn(io, argc, argv); respEnd(io); break; }
    }
    if (!c->name) { respError(io, "unknown command"); respEnd(io); }
    action = sAction;
  }

  sRxLines++;                          /* command processed → activity tick */
  if (sProtoMutex) osMutexRelease(sProtoMutex);
  return action;
}

/* ---- RX byte feed (ISR-safe: no blocking, emits nothing) ----------------- */
void ProtocolFeedByte(uint8_t b) {
  if (b == '\r' || b == '\n') {
    if (sLen > 0) {                           /* finalize a content line          */
      sLine[sLen] = '\0';
      memcpy(sReady, sLine, sLen + 1);
      sReadyFlag = 1;
      sLen = 0;
      sPendEOL = (char)b;
    } else if (sPendEOL && (char)b != sPendEOL) {
      sPendEOL = 0;                           /* swallow the 2nd half of \r\n      */
    } else {                                  /* deliberate empty line → repeat    */
      sReady[0] = '\0';
      sReadyFlag = 1;
      sPendEOL = 0;
    }
    return;
  }
  if (sLen < PROTOCOL_LINE_MAX) sLine[sLen++] = (char)b;  /* else: truncate to max */
  sPendEOL = 0;
}

uint8_t ProtocolLineReady(void) { return sReadyFlag; }

void ProtocolService(void) {
  if (!sReadyFlag) return;
  sReadyFlag = 0;
  proto_action_t action = ProtocolProcessLine(&sUartIo, sReady);

  if (action == PROTO_ACT_HANDOFF) {   /* ack is on the wire (TX blocks until TC); */
    osDelay(5);                        /* let the line settle, then hand off.      */
    EnterBootloader();                 /* JUMP to the bootloader (no return).      */
  }                                    /* BOOT_FLAG (set by `update`) selects CLI vs boot. */
}

uint32_t ProtocolActivity(void) { return sRxLines; }

/* RS-485 service task: drain the circular-DMA RX ring every millisecond and
 * feed the line assembler. DMA replaced the per-byte RX interrupt, which cost
 * ~5-10 us of HAL+RTOS work per byte and collapsed above ~500 kbaud alongside
 * the 100 kHz motion ISR (52% duty). Self-echo needs no guard on this board:
 * DE and /RE are tied, so our own transmissions never reach the receiver. */
static void protocolTask(void *arg) {
  (void)arg;
  uint8_t chunk[64];
  Rs485RxStart();
  for (;;) {
    osDelay(1);
    uint16_t n;
    while ((n = Rs485RxDrain(chunk, (uint16_t)sizeof(chunk))) != 0U) {
      for (uint16_t i = 0; i < n; i++) {
        ProtocolFeedByte(chunk[i]);
        if (sReadyFlag) ProtocolService();
      }
    }
  }
}

void ProtocolStart(UART_HandleTypeDef *huart, rampsSharedData_t *shared) {
  sUart = huart;
  sShared = shared;
  sLen = 0; sPendEOL = 0; sReadyFlag = 0; sRxLines = 0;
  sLine[0] = '\0'; sReady[0] = '\0'; sUartIo.last[0] = '\0';
  Rs485TxInit();
  sProtoMutex = osMutexNew(NULL);
  sTask = osThreadNew(protocolTask, NULL, &kProtoTaskAttr);
}
