#include "main.h"
#include "stm32f4xx_it.h"
#include "Ramps.h"

extern rampsHandler_t RampsData;

extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim11;
extern UART_HandleTypeDef huart1;

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/

void NMI_Handler(void)
{
  while (1)
  {
  }
}

void HardFault_Handler(void)
{
  while (1)
  {
  }
}

void MemManage_Handler(void)
{
  while (1)
  {
  }
}

void BusFault_Handler(void)
{
  while (1)
  {
  }
}

void UsageFault_Handler(void)
{
  while (1)
  {
  }
}

void DebugMon_Handler(void)
{
}

/******************************************************************************/
/* STM32F4xx Peripheral Interrupt Handlers                                    */
/******************************************************************************/

/**
  * @brief TIM1 break interrupt and TIM9 global interrupt.
  *
  * RAM-resident + register-only (no HAL/flash calls) so step generation
  * continues while flash is being erased/programmed (the vector table is also
  * relocated to RAM in main). Clear the TIM9 update flag and run the motion
  * ISR. TIM1-break is unused (TIM1 is an encoder — no interrupts enabled).
  */
RAM_FUNC void TIM1_BRK_TIM9_IRQHandler(void)
{
  if (TIM9->SR & TIM_SR_UIF) {
    TIM9->SR = ~TIM_SR_UIF;
    SynchroRefreshTimerIsr(&RampsData);
  }
}

/**
  * @brief TIM1 update interrupt and TIM10 global interrupt (both unused; the
  *        HAL handler clears anything spurious). htim1 guarded: before
  *        MX_TIM1_Init its Instance is NULL, and the HAL would then "handle"
  *        register garbage read from address 0 — the flash alias — and can
  *        WRITE back there, latching FLASH_SR error flags (PGSERR|PGPERR)
  *        that abort the next real flash erase. Found the hard way: the very
  *        first tick interrupt (TIM11, this shared vector's sibling below)
  *        fires inside HAL_Init, long before the TIM handles exist.
  */
void TIM1_UP_TIM10_IRQHandler(void)
{
  if (htim1.Instance != NULL) {
    HAL_TIM_IRQHandler(&htim1);
  }
}

/**
  * @brief TIM1 trigger/commutation and TIM11 global interrupt.
  *        TIM11 is the HAL timebase (see stm32f4xx_hal_timebase_tim.c) and
  *        fires from inside HAL_Init — see the NULL-Instance note above.
  *        TIM1 never enables trigger/commutation interrupts in encoder mode.
  */
void TIM1_TRG_COM_TIM11_IRQHandler(void)
{
  if (htim1.Instance != NULL) {
    HAL_TIM_IRQHandler(&htim1);
  }
  HAL_TIM_IRQHandler(&htim11);
}

/**
  * @brief USART1 global interrupt — byte-IT RX (Protocol.c) and the TC-driven
  *        RS-485 DE release (usart.c).
  */
void USART1_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart1);
}

/* SVC_Handler, PendSV_Handler and SysTick_Handler are provided by FreeRTOS
 * (port.c / cmsis_os2.c) and must NOT be defined here. */
