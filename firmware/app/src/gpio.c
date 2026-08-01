#include "gpio.h"

/**
 * Configure the GPIOs this bring-up image owns: the status LED and the two
 * W5500 control lines (nRST / nINT). SPI2 AF pins are configured by the SPI
 * MSP init, and the USART1 pins by the UART MSP init.
 */
void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO port clocks. GPIOH is needed for the HSE crystal pins (PH0/PH1). */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  /* --- Status LED (PA12, active low) — start OFF ------------------------- */
  HAL_GPIO_WritePin(USR_LED_GPIO_Port, USR_LED_Pin, USR_LED_OFF_STATE);
  GPIO_InitStruct.Pin   = USR_LED_Pin;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(USR_LED_GPIO_Port, &GPIO_InitStruct);

  /* --- W5500 SCSn (PB12) — software chip select, idle HIGH ----------------
   * This pin MUST be a plain GPIO output, not SPI2_NSS: the W5500 needs SCSn
   * held low for the whole VDM transaction. R72 pulls it up externally, so the
   * chip stays deselected between here and the first transfer. */
  HAL_GPIO_WritePin(W5500_CS_GPIO_Port, W5500_CS_Pin, GPIO_PIN_SET);
  GPIO_InitStruct.Pin   = W5500_CS_Pin;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(W5500_CS_GPIO_Port, &GPIO_InitStruct);

  /* --- W5500 nRST (PC7, active low) — hold the chip in reset for now ------
   * Net has a pull-up (R70); asserting it low here makes the reset pulse in
   * W5500_HardReset() deterministic regardless of how long the rails took. */
  HAL_GPIO_WritePin(W5500_RST_GPIO_Port, W5500_RST_Pin, GPIO_PIN_RESET);
  GPIO_InitStruct.Pin   = W5500_RST_Pin;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(W5500_RST_GPIO_Port, &GPIO_InitStruct);

  /* --- W5500 nINT (PC6, active low, open-drain at the W5500) --------------
   * Polled in this image (no EXTI yet); the external pull-up (R71) already
   * defines the idle level, so no internal pull is required. */
  GPIO_InitStruct.Pin  = W5500_INT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(W5500_INT_GPIO_Port, &GPIO_InitStruct);
}

void UsrLedSet(int on)
{
  HAL_GPIO_WritePin(USR_LED_GPIO_Port, USR_LED_Pin,
                    on ? USR_LED_ON_STATE : USR_LED_OFF_STATE);
}

void UsrLedToggle(void)
{
  HAL_GPIO_TogglePin(USR_LED_GPIO_Port, USR_LED_Pin);
}
