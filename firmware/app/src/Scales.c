/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Encoder scale timer configuration. Ported from the drdro-firmware-f4 baseline
 * with one deliberate difference: IC1Polarity is FALLING instead of RISING.
 *
 * The V1.5 board's AM26LV32E receivers deliver the differential pair inverted
 * (A+ lands on the B-input pin, consistently on all five channels), which flips
 * the quadrature count direction. Inverting the capture polarity of ONE channel
 * flips it back at zero runtime cost, so raw counts increase in the natural
 * direction and `scales.dir` stays a purely user-level preference.
 */
#include "Scales.h"

HAL_StatusTypeDef initScaleTimer(TIM_HandleTypeDef * timHandle, uint16_t filter)
{
  TIM_Encoder_InitTypeDef sConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  if (filter > SCALES_FILTER_MAX) filter = SCALES_FILTER_MAX;

  timHandle->Init.Prescaler = 0;
  timHandle->Init.CounterMode = TIM_COUNTERMODE_UP;
  timHandle->Init.Period = 65535;   /* uniform 16-bit window on all five timers
                                       (TIM2/TIM5 are 32-bit — the int16 delta
                                       math in the ISR relies on 65535 here) */
  timHandle->Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  timHandle->Init.RepetitionCounter = 0;
  timHandle->Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  sConfig.EncoderMode = TIM_ENCODERMODE_TI12;
  sConfig.IC1Polarity = TIM_ICPOLARITY_FALLING;   /* board-level A/B inversion fix */
  sConfig.IC1Selection = TIM_ICSELECTION_DIRECTTI;
  sConfig.IC1Prescaler = TIM_ICPSC_DIV1;
  sConfig.IC1Filter = filter;
  sConfig.IC2Polarity = TIM_ICPOLARITY_RISING;
  sConfig.IC2Selection = TIM_ICSELECTION_DIRECTTI;
  sConfig.IC2Prescaler = TIM_ICPSC_DIV1;
  sConfig.IC2Filter = filter;

  HAL_StatusTypeDef result = HAL_TIM_Encoder_Init(timHandle, &sConfig);
  if (result != HAL_OK) {
    return result;
  }

  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;

  result = HAL_TIMEx_MasterConfigSynchronization(timHandle, &sMasterConfig);
  return result;
}

/* Rewrite the ICxF bits of both encoder channels in place — no re-init, so the
 * counter keeps running and no counts are lost. */
void setScaleFilter(TIM_HandleTypeDef * timHandle, uint16_t filter)
{
  if (filter > SCALES_FILTER_MAX) filter = SCALES_FILTER_MAX;
  MODIFY_REG(timHandle->Instance->CCMR1,
             TIM_CCMR1_IC1F | TIM_CCMR1_IC2F,
             ((uint32_t)filter << TIM_CCMR1_IC1F_Pos) |
             ((uint32_t)filter << TIM_CCMR1_IC2F_Pos));
}
