/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Minimal YMODEM receiver — see ymodem.h. Implements the receiver half of the
 * classic YMODEM batch protocol in CRC-16 mode:
 *   - drive the transfer with 'C' (CRC request); ACK/NAK each packet
 *   - block 0 = header (filename\0size\0...); blocks 1.. = data (128 or 1024 B)
 *   - EOT handshake (NAK first EOT, ACK the second), then ACK the trailing
 *     null header that closes the batch
 * Data packets are streamed straight to flash (flash_program_write); the last packet's
 * CTRL-Z padding is written too (harmless — it lands past the real image).
 */
#include "ymodem.h"
#include "flash.h"
#include "bl_io.h"

#define SOH    0x01U   /* 128-byte packet */
#define STX    0x02U   /* 1024-byte packet */
#define EOT    0x04U
#define ACK    0x06U
#define NAK    0x15U
#define CAN    0x18U
#define CRC_C  0x43U   /* 'C' — request CRC-16 mode */

#define BYTE_TIMEOUT_MS  1000U
#define START_RETRIES    20      /* ~20 s of 'C' before giving up */

static UART_HandleTypeDef *s_huart;
static uint8_t s_data[1024];     /* one max-size packet */

static int rx(uint8_t *b, uint32_t to) {
  return HAL_UART_Receive(s_huart, b, 1U, to) == HAL_OK ? 0 : -1;
}
static void tx(uint8_t b) {
  /* bl_tx (main.c) wraps the byte in the PA11 DE assert/release for the SP3485. */
  bl_tx(&b, 1U);
}

static uint16_t crc16(const uint8_t *p, uint32_t n) {
  uint16_t c = 0;
  while (n--) {
    c ^= (uint16_t)(*p++) << 8;
    for (int i = 0; i < 8; i++)
      c = (c & 0x8000U) ? (uint16_t)((c << 1) ^ 0x1021U) : (uint16_t)(c << 1);
  }
  return c;
}

/* Receive one packet into s_data. Returns SOH/STX (with *blk,*len set), EOT, CAN,
 * or 0 on timeout / framing / CRC error. */
static int recv_packet(uint8_t *blk, uint32_t *len) {
  uint8_t hdr;
  if (rx(&hdr, BYTE_TIMEOUT_MS)) return 0;
  uint32_t dlen;
  if      (hdr == SOH) dlen = 128U;
  else if (hdr == STX) dlen = 1024U;
  else if (hdr == EOT) return EOT;
  else if (hdr == CAN) return CAN;
  else return 0;

  uint8_t b1, b2, ch, cl;
  if (rx(&b1, BYTE_TIMEOUT_MS) || rx(&b2, BYTE_TIMEOUT_MS)) return 0;
  for (uint32_t i = 0; i < dlen; i++)
    if (rx(&s_data[i], BYTE_TIMEOUT_MS)) return 0;
  if (rx(&ch, BYTE_TIMEOUT_MS) || rx(&cl, BYTE_TIMEOUT_MS)) return 0;

  if ((uint8_t)(b1 ^ b2) != 0xFFU) return 0;            /* blk vs ~blk */
  if (crc16(s_data, dlen) != (((uint16_t)ch << 8) | cl)) return 0;
  *blk = b1;
  *len = dlen;
  return (int)hdr;
}

/* Parse "size" from a YMODEM block-0 header (filename NUL size SP/NUL ...). */
static uint32_t parse_size(uint32_t len) {
  uint32_t i = 0;
  while (i < len && s_data[i]) i++;          /* skip filename */
  i++;                                       /* skip its NUL */
  uint32_t sz = 0;
  while (i < len && s_data[i] >= '0' && s_data[i] <= '9')
    sz = sz * 10U + (uint32_t)(s_data[i++] - '0');
  return sz;
}

int ymodem_receive(UART_HandleTypeDef *huart, uint32_t base, uint32_t sector,
                   uint32_t *out_size) {
  s_huart = huart;
  uint8_t  blk = 0;
  uint32_t len = 0;
  uint32_t filesize = 0;
  uint32_t written  = 0;       /* flash offset = total bytes programmed */
  uint8_t  expected = 1;       /* next data block # after the header */

  flash_program_begin(base, sector);

  /* Handshake: send 'C' until the first packet (block 0 header) arrives. */
  int r = 0;
  for (int tries = 0; tries < START_RETRIES; tries++) {
    tx(CRC_C);
    r = recv_packet(&blk, &len);
    if (r == SOH || r == STX || r == CAN) break;
  }
  if (r == CAN) { flash_program_end(); return YM_CANCELLED; }
  if (r != SOH && r != STX) { flash_program_end(); return YM_TIMEOUT; }

  /* Block 0: header. Empty filename here would mean an empty batch. */
  if (blk == 0) {
    if (s_data[0] == 0) { tx(ACK); flash_program_end(); return YM_TIMEOUT; }
    filesize = parse_size(len);
    tx(ACK);
    tx(CRC_C);                 /* request the first data packet */
    r = recv_packet(&blk, &len);
  }

  for (;;) {
    if (r == SOH || r == STX) {
      if (blk == expected) {
        if (flash_program_write(written, s_data, len)) {
          tx(CAN); tx(CAN); flash_program_end(); return YM_FLASH_ERR;
        }
        written += len;
        tx(ACK);
        expected++;
      } else if (blk == (uint8_t)(expected - 1U)) {
        tx(ACK);               /* duplicate of the last packet — re-ACK */
      } else {
        tx(NAK);               /* out of sequence */
      }
    } else if (r == EOT) {
      tx(NAK);                 /* YMODEM: NAK the first EOT... */
      uint8_t b;
      if (rx(&b, BYTE_TIMEOUT_MS) == 0 && b == EOT) {
        tx(ACK);               /* ...ACK the second */
        tx(CRC_C);             /* ask for the trailing null header */
        r = recv_packet(&blk, &len);
        if ((r == SOH || r == STX) && blk == 0) tx(ACK);
        break;                 /* transfer complete */
      }
    } else if (r == CAN) {
      flash_program_end();
      return YM_CANCELLED;
    } else {
      tx(NAK);                 /* timeout / framing / CRC error — request resend */
    }
    r = recv_packet(&blk, &len);
  }

  flash_program_end();
  if (out_size) *out_size = filesize ? filesize : written;
  return YM_OK;
}
