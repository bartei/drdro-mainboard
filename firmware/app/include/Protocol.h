/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * drDRO line protocol: text command in, key=value lines out, terminated by an
 * empty line (wire format identical to the drdro-firmware-f4 baseline — hosts
 * reuse their parser byte-for-byte).
 *
 * V1.5 port: the response path is abstracted behind proto_io_t so the SAME
 * parse/dispatch core serves both the RS-485 UART (byte-IT RX + DE-managed TX)
 * and the W5500 TCP CLI (NetCli.c). Each transport owns a proto_io_t instance:
 * a raw write sink, optional begin/end hooks (RS-485 turnaround settle / TCP
 * buffer flush), the running response CRC, and its own repeat-last buffer.
 */
#ifndef DRDRO_PROTOCOL_H
#define DRDRO_PROTOCOL_H

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "Ramps.h"                 /* rampsSharedData_t (the variable registry target) */

#ifndef FW_VERSION
#define FW_VERSION "dev"           /* overridden at build time by support/fw_version.py */
#endif

#define PROTOCOL_LINE_MAX 128      /* max command line length (excl. terminator) */

/* ---- transport abstraction ---------------------------------------------- */
typedef struct proto_io {
  /* raw byte sink (required). Called for every response fragment. */
  void (*writeFn)(struct proto_io *io, const char *s, uint16_t n);
  /* pre-response hook (optional): RS-485 RX->TX turnaround settle. */
  void (*beginFn)(struct proto_io *io);
  /* post-response hook (optional): TCP buffer flush. */
  void (*endFn)(struct proto_io *io);
  void   *user;                            /* transport context (e.g. socket #) */
  uint8_t crc;                             /* running XOR over the response body */
  char    last[PROTOCOL_LINE_MAX + 1];     /* previous command (empty-line repeat) */
} proto_io_t;

/* What the caller must do after the response has been fully written/flushed. */
typedef enum {
  PROTO_ACT_NONE = 0,
  PROTO_ACT_HANDOFF,   /* update/reset/rollback: hand control to the bootloader */
} proto_action_t;

/** Bind the UART (TX/RX) + shared-data block, start byte-IT RX and the service
 *  task. Call once from RampsStart(). */
void ProtocolStart(UART_HandleTypeDef *huart, rampsSharedData_t *shared);

/** Count of processed commands — used as a comm-activity tick. */
uint32_t ProtocolActivity(void);

/** Feed one received byte; assembles a line and flags it ready on \r / \n.
 *  Safe to call from the UART RX ISR (no blocking, no response emitted here). */
void ProtocolFeedByte(uint8_t b);

/** True once a complete line is buffered and ready to process. */
uint8_t ProtocolLineReady(void);

/** Process the most recently completed UART line: parse, dispatch, emit the
 *  response. Runs in task context (does blocking TX). Clears the ready flag. */
void ProtocolService(void);

/** Parse + dispatch + respond for an explicit line (mutated in place) on the
 *  given transport. Exposed for the TCP CLI and for host-side tests. Returns
 *  the post-response action the transport must perform. */
proto_action_t ProtocolProcessLine(proto_io_t *io, char *line);

/** Hand off to the IAP bootloader by jumping (not resetting) — implemented in
 *  main.c. The UART transport performs this for PROTO_ACT_HANDOFF. */
void EnterBootloader(void);

#endif /* DRDRO_PROTOCOL_H */
