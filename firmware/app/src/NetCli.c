/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * TCP CLI server — see NetCli.h. Runs the transport-independent protocol core
 * (Protocol.c ProtocolProcessLine) over W5500 socket 1.
 *
 * Responses are accumulated in a local buffer and pushed with send() when full
 * and at respEnd (the endFn hook) — one segment per response in the common
 * case, instead of one per key=value fragment. The wizchip register access
 * underneath send/recv is serialized against the net task by the recursive
 * mutex installed in Net.c's cris callbacks.
 *
 * `update`/`reset`/`rollback` over TCP: the response is flushed, the socket is
 * closed, and the board resets via NVIC_SystemReset() — safe on this board
 * (BOOT0 is pulled down) and it lands in the bootloader at 0x08000000, which
 * honors BOOT_FLAG / the persisted bank selection exactly like a jump.
 */
#include <string.h>
#include "cmsis_os2.h"
#include "NetCli.h"
#include "Net.h"
#include "Protocol.h"
#include "socket.h"

#define CLI_SOCKET        1U
#define CLI_PORT_DEFAULT  5555U
#define CLI_POLL_MS       20U   /* idle poll: connection/link state changes    */
#define CLI_POLL_HOT_MS   1U    /* active poll: bytes arrived on the last pass */

static rampsSharedData_t *sShared = NULL;

static const osThreadAttr_t kNetCliTaskAttr = {
  .name = "netcli", .stack_size = 768 * 4, .priority = (osPriority_t) osPriorityNormal,
};

/* ---- buffered TCP response sink ------------------------------------------ */
static uint8_t sTxBuf[512];
static uint16_t sTxLen = 0;

static void tcpFlush(void)
{
  /* ioLibrary send() returns SOCK_BUSY (0) while the PREVIOUS send is still
   * waiting for its SENDOK (data not yet ACKed by the peer) — routine on a
   * multi-flush response at 10BASE-T, NOT an error. Treating it as fatal is
   * what clipped every response at the first buffer boundary (512 B): retry
   * until it drains. Negative = real error (socket closed/reset) — drop the
   * rest. A deadline guards against a frozen peer (zero-window) parking this
   * task (and, via the protocol mutex, the RS-485 CLI) forever: drop the
   * connection instead. */
  uint16_t off = 0;
  uint32_t deadline = osKernelGetTickCount() + 2000U;
  while (off < sTxLen) {
    int32_t n = send(CLI_SOCKET, sTxBuf + off, (uint16_t)(sTxLen - off));
    if (n == SOCK_BUSY) {
      if ((int32_t)(osKernelGetTickCount() - deadline) > 0) {
        disconnect(CLI_SOCKET);             /* frozen peer — abandon the connection */
        break;
      }
      osDelay(1);
      continue;
    }
    if (n < 0) break;                       /* connection died — drop the rest */
    off += (uint16_t)n;
  }
  sTxLen = 0;
}
static void tcpIoWrite(proto_io_t *io, const char *s, uint16_t n)
{
  (void)io;
  while (n) {
    uint16_t room = (uint16_t)(sizeof(sTxBuf) - sTxLen);
    if (room == 0U) { tcpFlush(); continue; }
    uint16_t chunk = (n < room) ? n : room;
    memcpy(sTxBuf + sTxLen, s, chunk);
    sTxLen += chunk;
    s += chunk;
    n -= chunk;
  }
}
static void tcpIoEnd(proto_io_t *io)
{
  (void)io;
  tcpFlush();
}
static proto_io_t sTcpIo = { tcpIoWrite, NULL, tcpIoEnd, NULL, 0, {0} };

/* ---- per-connection line assembly (same semantics as ProtocolFeedByte) --- */
static char     sLine[PROTOCOL_LINE_MAX + 1];
static uint16_t sLen = 0;
static char     sPendEOL = 0;

/* Feed one byte; returns 1 when a complete line (possibly empty = repeat) is
 * ready in out[]. Mirrors the UART CRLF handling so `\r\n` is one terminator
 * but a deliberate blank line still triggers the repeat-last behavior. */
static int feedByte(uint8_t b, char *out)
{
  if (b == '\r' || b == '\n') {
    if (sLen > 0) {
      sLine[sLen] = '\0';
      memcpy(out, sLine, sLen + 1U);
      sLen = 0;
      sPendEOL = (char)b;
      return 1;
    }
    if (sPendEOL && (char)b != sPendEOL) {
      sPendEOL = 0;                          /* swallow the 2nd half of \r\n */
      return 0;
    }
    out[0] = '\0';                           /* deliberate empty line → repeat */
    sPendEOL = 0;
    return 1;
  }
  if (sLen < PROTOCOL_LINE_MAX) sLine[sLen++] = (char)b;
  sPendEOL = 0;
  return 0;
}

static void resetConnState(void)
{
  sLen = 0;
  sPendEOL = 0;
  sTxLen = 0;
}

static _Noreturn void netCliTask(void *arg)
{
  (void)arg;
  uint8_t rxChunk[128];
  char    line[PROTOCOL_LINE_MAX + 1];
  uint8_t open = 0;
  uint8_t hot = 0;    /* data seen on the last pass — poll fast for a while */

  for (;;) {
    if (NetGetState() != NET_STATE_LEASED) {
      if (open) { close(CLI_SOCKET); open = 0; }
      osDelay(200);
      continue;
    }

    uint16_t port = sShared->net.port ? sShared->net.port : CLI_PORT_DEFAULT;

    if (!open) {
      if (socket(CLI_SOCKET, Sn_MR_TCP, port, 0) == CLI_SOCKET &&
          listen(CLI_SOCKET) == SOCK_OK) {
        open = 1;
      } else {
        close(CLI_SOCKET);
        osDelay(500);
        continue;
      }
    }

    switch (getSn_SR(CLI_SOCKET)) {
      case SOCK_ESTABLISHED: {
        int32_t avail = (int32_t)getSn_RX_RSR(CLI_SOCKET);
        /* Adaptive cadence: a client mid-conversation gets a 1 ms poll (the
         * sequential request rate is limited by this, not the wire); an idle
         * connection decays back to the relaxed poll after ~100 quiet passes. */
        if (avail > 0)      hot = 100;
        else if (hot > 0)   hot--;
        while (avail > 0) {
          int32_t n = recv(CLI_SOCKET, rxChunk,
                           (avail > (int32_t)sizeof(rxChunk)) ? (uint16_t)sizeof(rxChunk)
                                                              : (uint16_t)avail);
          if (n <= 0) break;
          for (int32_t i = 0; i < n; i++) {
            if (!feedByte(rxChunk[i], line)) continue;
            proto_action_t action = ProtocolProcessLine(&sTcpIo, line);
            if (action == PROTO_ACT_HANDOFF) {
              /* response already flushed by respEnd's endFn */
              disconnect(CLI_SOCKET);
              osDelay(50);                   /* let the FIN/ACK drain */
              NVIC_SystemReset();            /* BOOT0 tied low: lands in the bootloader */
            }
          }
          avail -= n;
        }
        break;
      }
      case SOCK_CLOSE_WAIT:
        disconnect(CLI_SOCKET);
        resetConnState();
        break;
      case SOCK_CLOSED:
        open = 0;                            /* relisten on the next pass */
        resetConnState();
        break;
      default:
        break;                               /* LISTEN/SYNRECV/FIN states: wait */
    }

    osDelay(hot ? CLI_POLL_HOT_MS : CLI_POLL_MS);
  }
}

void NetCliStart(rampsSharedData_t *shared)
{
  sShared = shared;
  osThreadNew(netCliTask, NULL, &kNetCliTaskAttr);
}
