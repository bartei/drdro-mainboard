/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Encoder scale timers — V1.5 mainboard: 5 hardware-quadrature channels on
 * TIM1..TIM5 (see shared/BoardPins.h). Ported from the drdro-firmware-f4
 * baseline (4 channels, TIM1..TIM4).
 */
#ifndef DRDRO_SCALES_H
#define DRDRO_SCALES_H
#include "stm32f4xx_hal.h"

#define SCALES_COUNT 5

/* TIM input-capture filter (CCMR1 ICxF) is a 4-bit field: 0 (off) .. 15 (max). */
#define SCALES_FILTER_MAX 15U

HAL_StatusTypeDef initScaleTimer(TIM_HandleTypeDef * timHandle, uint16_t filter);

/* Reprogram the encoder input filter (both channels) on a running timer. */
void setScaleFilter(TIM_HandleTypeDef * timHandle, uint16_t filter);
#endif /* DRDRO_SCALES_H */
