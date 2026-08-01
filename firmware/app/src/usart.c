#include "usart.h"
#include "cmsis_os2.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

UART_HandleTypeDef huart1;
DMA_HandleTypeDef  hdma_usart1_rx;

/* Signalled by HAL_UART_TxCpltCallback (TC: last stop bit on the wire). */
static osSemaphoreId_t sTxDone = NULL;

/* Circular RX ring, drained by the protocol task (~1 ms cadence). DMA moves
 * every received byte here with ZERO per-byte CPU — the byte-interrupt scheme
 * it replaces needed ~5-10 us of HAL+RTOS work per byte and collapsed above
 * ~500 kbaud once the 100 kHz motion ISR (52% duty) competed for the core. */
static uint8_t  sRxRing[RS485_RX_RING];
static uint16_t sRxTail = 0;   /* next unread index (task side) */

/* USART1 @115200 8N1 — the RS-485 host link (U5 = SP3485EN).
 * USART1 is on APB2 (100 MHz), so the baud divisor is exact at 115200 given the
 * 16 MHz HSE -> 100 MHz SYSCLK configured in SystemClock_Config(). */
void MX_USART1_UART_Init(void)
{
  huart1.Instance          = RS485_UART;
  huart1.Init.BaudRate     = 115200;
  huart1.Init.WordLength   = UART_WORDLENGTH_8B;
  huart1.Init.StopBits     = UART_STOPBITS_1;
  huart1.Init.Parity       = UART_PARITY_NONE;
  huart1.Init.Mode         = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
}

void HAL_UART_MspInit(UART_HandleTypeDef *uartHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if (uartHandle->Instance == RS485_UART)
  {
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /**USART1 GPIO Configuration
    PA10     ------> USART1_RX
    PA15     ------> USART1_TX
    */
    GPIO_InitStruct.Pin       = RS485_TX_Pin | RS485_RX_Pin;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_NOPULL;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = RS485_UART_AF;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* RS-485 driver enable. The F411 has no hardware DEM, so DE is a plain
     * GPIO: assert before TX, release once the last stop bit is out (TC).
     * Idle LOW = receiver enabled (/RE and DE are tied together at U5). */
    HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);
    GPIO_InitStruct.Pin   = RS485_DE_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(RS485_DE_GPIO_Port, &GPIO_InitStruct);

    /* RX by circular DMA: USART1_RX = DMA2 Stream 2, Channel 4. Byte-wide,
     * peripheral-to-memory, circular over sRxRing; the protocol task polls the
     * write position (NDTR), so the stream needs NO interrupts at all. */
    __HAL_RCC_DMA2_CLK_ENABLE();
    hdma_usart1_rx.Instance                 = DMA2_Stream2;
    hdma_usart1_rx.Init.Channel             = DMA_CHANNEL_4;
    hdma_usart1_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_usart1_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_usart1_rx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_usart1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart1_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_usart1_rx.Init.Mode                = DMA_CIRCULAR;
    hdma_usart1_rx.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_usart1_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_usart1_rx) != HAL_OK)
    {
      Error_Handler();
    }
    __HAL_LINKDMA(uartHandle, hdmarx, hdma_usart1_rx);

    /* USART1 interrupt: TC-driven DE release (Rs485Send) + error recovery.
     * Priority 15: the ISR calls RTOS APIs (>= 5 rule). */
    HAL_NVIC_SetPriority(USART1_IRQn, 15, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
  }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *uartHandle)
{
  if (uartHandle->Instance == RS485_UART)
  {
    __HAL_RCC_USART1_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOA, RS485_TX_Pin | RS485_RX_Pin);
  }
}

/**
 * Protocol transmit over the half-duplex RS-485 link. Asserts DE (which also
 * mutes our receiver — /RE is tied to DE at U5), starts an interrupt-driven
 * transmit, and blocks the CALLING TASK on a semaphore until the TC interrupt
 * fires; the DE release happens in the TxCplt callback, i.e. exactly when the
 * last stop bit is on the wire. The caller's buffer stays valid throughout
 * because the caller is parked until completion.
 *
 * Task context only (after ProtocolStart). Early-boot code uses DebugPrint.
 */
void Rs485Send(const uint8_t *data, uint16_t len)
{
  if (len == 0U) return;
  if (sTxDone == NULL) {                 /* protocol not started yet — fall back */
    HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_SET);
    HAL_UART_Transmit(&huart1, (uint8_t *)data, len, 1000U);
    HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);
    return;
  }
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_SET);
  if (HAL_UART_Transmit_IT(&huart1, (uint8_t *)data, len) != HAL_OK) {
    HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);
    return;
  }
  osSemaphoreAcquire(sTxDone, osWaitForever);
}

/* TC interrupt: drop DE first (bounded turnaround), then release the sender. */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance != RS485_UART) return;
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);
  if (sTxDone != NULL) osSemaphoreRelease(sTxDone);
}

/* Create the TX-done semaphore once the kernel objects can exist (called from
 * ProtocolStart via Rs485TxInit). */
void Rs485TxInit(void)
{
  if (sTxDone == NULL) sTxDone = osSemaphoreNew(1U, 0U, NULL);
}

/* Start (or restart) circular-DMA reception into the ring. */
void Rs485RxStart(void)
{
  sRxTail = 0;
  HAL_UART_DMAStop(&huart1);
  HAL_UART_Receive_DMA(&huart1, sRxRing, RS485_RX_RING);
}

/* Drain bytes the DMA has written since the last call. Returns the number of
 * bytes copied into out[] (0 if none). Task context. */
uint16_t Rs485RxDrain(uint8_t *out, uint16_t max)
{
  uint16_t head = (uint16_t)(RS485_RX_RING - __HAL_DMA_GET_COUNTER(huart1.hdmarx));
  uint16_t n = 0;
  while (sRxTail != head && n < max) {
    out[n++] = sRxRing[sRxTail];
    sRxTail = (uint16_t)((sRxTail + 1U) % RS485_RX_RING);
  }
  return n;
}

/* RX line errors (overrun/noise/framing): with circular DMA an overrun cannot
 * normally happen (DMA drains RDR autonomously), but electrical noise can still
 * raise NE/FE and the HAL then ABORTS the DMA reception. Clear and restart so
 * the console can never die. */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance != RS485_UART) return;
  __HAL_UART_CLEAR_OREFLAG(huart);
  __HAL_UART_CLEAR_NEFLAG(huart);
  __HAL_UART_CLEAR_FEFLAG(huart);
  huart->ErrorCode = HAL_UART_ERROR_NONE;
  sRxTail = 0;
  HAL_UART_Receive_DMA(huart, sRxRing, RS485_RX_RING);
}

/**
 * Blocking debug transmit. HAL_UART_Transmit() returns only once TC is set, so
 * releasing DE immediately afterwards cannot truncate the frame.
 */
void DebugPrint(const char *s)
{
  size_t len = strlen(s);
  if (len == 0U)
  {
    return;
  }

  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_SET);
  HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)len, 1000U);
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);
}

void DebugPrintf(const char *fmt, ...)
{
  char buf[128];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  DebugPrint(buf);
}
