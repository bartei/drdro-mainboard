/*
 * Minimal host-side mock of the STM32 HAL — only what Protocol.c / Ramps.h /
 * Scales.h / usart.h / BoardPins.h need to compile for native unit tests.
 * GPIO writes are captured via MockGpioWrite() (defined in the test) so the
 * `dout` command can be asserted.
 */
#ifndef MOCK_STM32F4XX_HAL_H
#define MOCK_STM32F4XX_HAL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
#define HAL_MAX_DELAY 0xFFFFFFFFU

typedef struct { uint32_t dummy; volatile uint32_t BSRR; } GPIO_TypeDef;  /* BSRR: fast gpioSet/Reset */
typedef struct { void *Instance; uint32_t dummy; } TIM_HandleTypeDef;
typedef struct { void *Instance; uint32_t dummy; } UART_HandleTypeDef;

typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET } GPIO_PinState;

/* Pin masks + port instances (BoardPins.h expands to these). */
#define GPIO_PIN_0   ((uint16_t)0x0001)
#define GPIO_PIN_1   ((uint16_t)0x0002)
#define GPIO_PIN_2   ((uint16_t)0x0004)
#define GPIO_PIN_3   ((uint16_t)0x0008)
#define GPIO_PIN_4   ((uint16_t)0x0010)
#define GPIO_PIN_5   ((uint16_t)0x0020)
#define GPIO_PIN_6   ((uint16_t)0x0040)
#define GPIO_PIN_7   ((uint16_t)0x0080)
#define GPIO_PIN_8   ((uint16_t)0x0100)
#define GPIO_PIN_9   ((uint16_t)0x0200)
#define GPIO_PIN_10  ((uint16_t)0x0400)
#define GPIO_PIN_11  ((uint16_t)0x0800)
#define GPIO_PIN_12  ((uint16_t)0x1000)
#define GPIO_PIN_13  ((uint16_t)0x2000)
#define GPIO_PIN_14  ((uint16_t)0x4000)
#define GPIO_PIN_15  ((uint16_t)0x8000)

extern GPIO_TypeDef MockGpioA, MockGpioB, MockGpioC;
#define GPIOA (&MockGpioA)
#define GPIOB (&MockGpioB)
#define GPIOC (&MockGpioC)

/* AF macros referenced by BoardPins.h (values irrelevant on host). */
#define GPIO_AF1_TIM1    1U
#define GPIO_AF1_TIM2    1U
#define GPIO_AF2_TIM3    2U
#define GPIO_AF2_TIM4    2U
#define GPIO_AF2_TIM5    2U
#define GPIO_AF4_I2C1    4U
#define GPIO_AF7_USART1  7U
#define GPIO_AF5_SPI2    5U
#define SPI2             ((void *)0)
#define USART1           ((void *)0)
#define I2C1             ((void *)0)

/* Test hook: capture GPIO writes (dout command). */
void MockGpioWrite(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState st);
static inline void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState st) {
  MockGpioWrite(port, pin, st);
}

/* Test hook: capture transmitted bytes (kept for HAL-level TX paths). */
void MockUartCapture(const uint8_t *data, uint16_t size);

static inline HAL_StatusTypeDef
HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t to) {
  (void)h; (void)to;
  MockUartCapture(d, n);
  return HAL_OK;
}
static inline HAL_StatusTypeDef
HAL_UART_Receive_IT(UART_HandleTypeDef *h, uint8_t *d, uint16_t n) {
  (void)h; (void)d; (void)n;
  return HAL_OK;
}

/* CMSIS core: no-op on host (the reset/handoff paths are hardware-only). */
static inline void NVIC_SystemReset(void) { }

#endif /* MOCK_STM32F4XX_HAL_H */
