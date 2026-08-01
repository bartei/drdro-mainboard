#ifndef __USART_H__
#define __USART_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

extern UART_HandleTypeDef huart1;

#define RS485_RX_RING 512U   /* circular DMA RX buffer (must outlast 1 task poll) */

void MX_USART1_UART_Init(void);

/* Protocol TX: DE asserted, interrupt-driven transmit, DE released on TC.
 * Blocks the calling task until the frame is fully on the wire. */
void Rs485Send(const uint8_t *data, uint16_t len);

/* Create the TX-done semaphore (called from ProtocolStart). */
void Rs485TxInit(void);

/* Circular-DMA RX: start reception / drain newly received bytes (task side). */
void     Rs485RxStart(void);
uint16_t Rs485RxDrain(uint8_t *out, uint16_t max);

/* Blocking debug print over the RS-485 link (drives DE around the transfer).
 * EARLY BOOT ONLY — once ProtocolStart has run, the protocol owns USART1. */
void DebugPrint(const char *s);
void DebugPrintf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif /* __USART_H__ */
