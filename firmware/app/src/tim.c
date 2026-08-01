/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Timer init — V1.5 mainboard. Five hardware-quadrature encoder timers
 * (TIM1..TIM5, pins per shared/BoardPins.h) and TIM9 as the 100 kHz motion
 * base timer. Ported from the drdro-firmware-f4 baseline (which had TIM1..4);
 * TIM5 is new (ENC5 on PA0/PA1, AF2 — deliberately NOT TIM2, which is ENC2).
 *
 * The encoder configuration set here is re-applied by initScaleTimer() in
 * RampsStart (with the persisted filter value), so keep the two in sync —
 * notably the FALLING IC1 polarity that compensates the AM26LV32E receiver
 * inversion (see Scales.c).
 */
#include "tim.h"

TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;
TIM_HandleTypeDef htim4;
TIM_HandleTypeDef htim5;
TIM_HandleTypeDef htim9;

/* Shared encoder-mode init: x4 quadrature (TI12), 16-bit window on every timer
 * (TIM2/TIM5 are 32-bit — the ISR's int16 delta math relies on Period=65535). */
static void encoderInit(TIM_HandleTypeDef *h, TIM_TypeDef *instance)
{
  TIM_Encoder_InitTypeDef sConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  h->Instance = instance;
  h->Init.Prescaler = 0;
  h->Init.CounterMode = TIM_COUNTERMODE_UP;
  h->Init.Period = 65535;
  h->Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  h->Init.RepetitionCounter = 0;
  h->Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  sConfig.EncoderMode = TIM_ENCODERMODE_TI12;
  sConfig.IC1Polarity = TIM_ICPOLARITY_FALLING;   /* AM26LV32E inversion fix (Scales.c) */
  sConfig.IC1Selection = TIM_ICSELECTION_DIRECTTI;
  sConfig.IC1Prescaler = TIM_ICPSC_DIV1;
  sConfig.IC1Filter = 0;
  sConfig.IC2Polarity = TIM_ICPOLARITY_RISING;
  sConfig.IC2Selection = TIM_ICSELECTION_DIRECTTI;
  sConfig.IC2Prescaler = TIM_ICPSC_DIV1;
  sConfig.IC2Filter = 0;
  if (HAL_TIM_Encoder_Init(h, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(h, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

void MX_TIM1_Init(void) { encoderInit(&htim1, TIM1); }
void MX_TIM2_Init(void) { encoderInit(&htim2, TIM2); }
void MX_TIM3_Init(void) { encoderInit(&htim3, TIM3); }
void MX_TIM4_Init(void) { encoderInit(&htim4, TIM4); }
void MX_TIM5_Init(void) { encoderInit(&htim5, TIM5); }

/* TIM9: motion base timer. APB2 timer clock = 100 MHz; PSC 99 + ARR 9 =>
 * 100 MHz / 100 / 10 = 100 kHz update interrupt (SynchroRefreshTimerIsr). */
void MX_TIM9_Init(void)
{
  TIM_ClockConfigTypeDef sClockSourceConfig = {0};

  htim9.Instance = TIM9;
  htim9.Init.Prescaler = 100 - 1;
  htim9.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim9.Init.Period = 10 - 1;
  htim9.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim9.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim9) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim9, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

void HAL_TIM_Encoder_MspInit(TIM_HandleTypeDef* tim_encoderHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

  if(tim_encoderHandle->Instance==TIM1)
  {
    __HAL_RCC_TIM1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    /* ENC1: PA8 = TIM1_CH1, PA9 = TIM1_CH2 */
    GPIO_InitStruct.Pin = ENC1_A_Pin | ENC1_B_Pin;
    GPIO_InitStruct.Alternate = ENC1_AF;
    HAL_GPIO_Init(ENC1_GPIO_Port, &GPIO_InitStruct);

    /* Shared TIM1/TIM9..11 vectors: TIM9 (motion ISR) at prio 5 — register-only
     * RAM_FUNC handler, no RTOS calls, so it may sit at the RTOS mask boundary.
     * TIM10 (unused) and TIM11 (HAL timebase) at the lowest priority. */
    HAL_NVIC_SetPriority(TIM1_BRK_TIM9_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIM1_BRK_TIM9_IRQn);
    HAL_NVIC_SetPriority(TIM1_UP_TIM10_IRQn, 15, 0);
    HAL_NVIC_EnableIRQ(TIM1_UP_TIM10_IRQn);
    HAL_NVIC_SetPriority(TIM1_TRG_COM_TIM11_IRQn, 15, 0);
    HAL_NVIC_EnableIRQ(TIM1_TRG_COM_TIM11_IRQn);
  }
  else if(tim_encoderHandle->Instance==TIM2)
  {
    __HAL_RCC_TIM2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    /* ENC2: PA5 = TIM2_CH1, PB3 = TIM2_CH2 */
    GPIO_InitStruct.Pin = ENC2_A_Pin;
    GPIO_InitStruct.Alternate = ENC2_AF;
    HAL_GPIO_Init(ENC2_A_GPIO_Port, &GPIO_InitStruct);
    GPIO_InitStruct.Pin = ENC2_B_Pin;
    HAL_GPIO_Init(ENC2_B_GPIO_Port, &GPIO_InitStruct);
  }
  else if(tim_encoderHandle->Instance==TIM3)
  {
    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    /* ENC3: PA6 = TIM3_CH1, PA7 = TIM3_CH2 */
    GPIO_InitStruct.Pin = ENC3_A_Pin | ENC3_B_Pin;
    GPIO_InitStruct.Alternate = ENC3_AF;
    HAL_GPIO_Init(ENC3_GPIO_Port, &GPIO_InitStruct);
  }
  else if(tim_encoderHandle->Instance==TIM4)
  {
    __HAL_RCC_TIM4_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    /* ENC4: PB6 = TIM4_CH1, PB7 = TIM4_CH2 */
    GPIO_InitStruct.Pin = ENC4_A_Pin | ENC4_B_Pin;
    GPIO_InitStruct.Alternate = ENC4_AF;
    HAL_GPIO_Init(ENC4_GPIO_Port, &GPIO_InitStruct);
  }
  else if(tim_encoderHandle->Instance==TIM5)
  {
    __HAL_RCC_TIM5_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    /* ENC5: PA0 = TIM5_CH1, PA1 = TIM5_CH2 (AF2 — NOT TIM2, that's ENC2) */
    GPIO_InitStruct.Pin = ENC5_A_Pin | ENC5_B_Pin;
    GPIO_InitStruct.Alternate = ENC5_AF;
    HAL_GPIO_Init(ENC5_GPIO_Port, &GPIO_InitStruct);
  }
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef* tim_baseHandle)
{
  if(tim_baseHandle->Instance==TIM9)
  {
    __HAL_RCC_TIM9_CLK_ENABLE();
    HAL_NVIC_SetPriority(TIM1_BRK_TIM9_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIM1_BRK_TIM9_IRQn);
  }
}
