/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Motion core — ported from the drdro-firmware-f4 baseline. V1.5 board diffs:
 *   - 5 encoder scales (TIM1..TIM5) instead of 4
 *   - motor pins M1_STEP/M1_DIR/M_ENA on GPIOC via the ULN2003 (BoardPins.h);
 *     M2/M3 STEP/DIR configured as idle outputs (driven by `dout` only)
 *   - per-scale user direction flip (scales.dir) applied in the ISR
 *   - no SPARE pins (their PA0/PA1/PA3/PA4 are encoder/free pins here); the
 *     baseline's SPARE_2 step-mirror and its port/pin-mismatch bug are dropped
 */
#include <math.h>
#include "Ramps.h"
#include "Scales.h"
#include "Protocol.h"

uint16_t servoCycles = 0;
uint16_t servoCyclesCounter = 0;

const osThreadAttr_t ledTaskAttributes = {
.name = "UpdateLedTask",
.stack_size = 128 * 4,
.priority = (osPriority_t) osPriorityLow,
};

const osThreadAttr_t speedTaskAttributes = {
.name = "updateSpeedTask",
.stack_size = 128 * 4,
.priority = (osPriority_t) osPriorityLow,
};

const osThreadAttr_t servoEnableTaskAttributes = {
.name = "servoEnableTask",
.stack_size = 128 * 4,
.priority = (osPriority_t) osPriorityLow,
};

void configureOutputPin(GPIO_TypeDef *Port, uint16_t Pin) {
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin : PtPin */
  GPIO_InitStruct.Pin = Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(Port, &GPIO_InitStruct);
}

void RampsStart(rampsHandler_t *rampsData) {
  rampsData->shared.servo.maxSpeed = 720;
  rampsData->shared.servo.acceleration = 120;
  rampsData->shared.servo.indexSpeed = 0;   // 0 → indexing ramps fall back to maxSpeed

  for (int i = 0; i < SCALES_COUNT; i++) {
    rampsData->shared.scales[i].syncRatioNum = 1;
    rampsData->shared.scales[i].syncRatioDen = 100;
    rampsData->shared.scales[i].filterValue = 5;   // matches settings_defaults()
    rampsData->shared.scales[i].dirInvert = 0;
  }
  rampsData->shared.din.debounceMs = 5;            // matches settings_defaults()
  rampsData->shared.scaleCount = SCALES_COUNT;     // board capability, RO in the registry
  rampsData->shared.comBaud = 115200;              // matches settings_defaults()

  // Network compiled defaults (all must match settings_defaults()): DHCP on,
  // CLI port 5555, /24 static mask placeholder. Without these a factory-blank
  // board (no valid settings image) would come up in static mode at 0.0.0.0.
  rampsData->shared.net.dhcp = 1;
  rampsData->shared.net.port = 5555;
  rampsData->shared.net.cfgMask[0] = 255;
  rampsData->shared.net.cfgMask[1] = 255;
  rampsData->shared.net.cfgMask[2] = 255;
  rampsData->shared.net.cfgMask[3] = 0;

  // Configure motion pins (M1 = the servo axis; ENA is shared by all motors)
  configureOutputPin(DIR_GPIO_PORT, DIR_PIN);
  configureOutputPin(ENA_GPIO_PORT, ENA_PIN);
  configureOutputPin(STEP_GPIO_PORT, STEP_PIN);
  // M2/M3 exist on the board but are not motion-controlled yet: configure them
  // as plain outputs, idle low, so `dout` can exercise them during bring-up.
  configureOutputPin(MOTOR_GPIO_Port, M2_STEP_Pin);
  configureOutputPin(MOTOR_GPIO_Port, M2_DIR_Pin);
  configureOutputPin(MOTOR_GPIO_Port, M3_STEP_Pin);
  configureOutputPin(MOTOR_GPIO_Port, M3_DIR_Pin);

  // Configure tasks
  osThreadNew(userLedTask, rampsData, &ledTaskAttributes);
  osThreadNew(updateSpeedTask, rampsData, &speedTaskAttributes);
  osThreadNew(servoEnableTask, rampsData, &servoEnableTaskAttributes);

  // Initialize and start encoder timers, reset the sync flags
  for (int j = 0; j < SCALES_COUNT; ++j) {
    initScaleTimer(rampsData->shared.scales[j].timerHandle, rampsData->shared.scales[j].filterValue);
    HAL_TIM_Encoder_Start(rampsData->shared.scales[j].timerHandle, TIM_CHANNEL_ALL);
  }

  // Enable debug cycle counter
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  // Start the line protocol: owns USART1 RX + a service task
  ProtocolStart(rampsData->commUart, &rampsData->shared);

  // Start synchro interrupt
  HAL_TIM_Base_Start_IT(rampsData->synchroRefreshTimer);
}

static inline void
deltaPositionAndError(int32_t currentValue, int32_t ratioNum, int32_t ratioDen, deltaPosError_t *data) {
  int32_t startValue = (int16_t) (currentValue - data->oldPosition) * ratioNum + data->error;
  data->oldPosition = currentValue;
  data->scaledDelta = (int32_t) (startValue / ratioDen);
  data->error = (int32_t) (startValue % ratioDen);
}

static inline void updateIndexingPosition(rampsHandler_t *data) {
  rampsSharedData_t *shared = &(data->shared);
  float interval = (float) shared->executionInterval / 100000000.0f;
  float stopDistance = (shared->servo.currentSpeed * shared->servo.currentSpeed / shared->servo.acceleration) / 2;

  /* Indexing/offset moves ramp to their own feedrate cap, NOT the mechanical maxSpeed.
   * maxSpeed stays at the machine's true ceiling so the step-pulse cadence (servoCycles,
   * sized from maxSpeed in updateSpeedTask) — and therefore a simultaneous sync follower —
   * is never throttled by an indexing move. 0 / out-of-range falls back to maxSpeed. */
  float cap = shared->servo.indexSpeed;
  if (cap <= 0.0f || cap > shared->servo.maxSpeed) cap = shared->servo.maxSpeed;

  // Accelerate Pos
  if (shared->servo.stepsToGo > 0) {
    if ((float)shared->servo.stepsToGo > stopDistance && shared->servo.currentSpeed < cap) {
      shared->servo.currentSpeed += shared->servo.acceleration * interval;
      // max speed
      if (shared->servo.currentSpeed > cap) {
        shared->servo.currentSpeed = cap;
      }
    }

    // Decelerate Pos
    if ((float)shared->servo.stepsToGo < stopDistance) {
      shared->servo.currentSpeed -= shared->servo.acceleration * interval;
      if (shared->servo.currentSpeed < 0) {
        shared->servo.currentSpeed = 0;
      }
    }
  }

  if (shared->servo.stepsToGo < 0) {
    // Accelerate Neg
    if (-(float)shared->servo.stepsToGo > stopDistance && -shared->servo.currentSpeed < cap) {
      shared->servo.currentSpeed -= shared->servo.acceleration * interval;
      if (-shared->servo.currentSpeed > cap) {
        shared->servo.currentSpeed = -cap;
      }
    }

    // Decelerate Neg
    if (-(float)shared->servo.stepsToGo < stopDistance) {
      shared->servo.currentSpeed += shared->servo.acceleration * interval;
      if (shared->servo.currentSpeed > 0) {
        shared->servo.currentSpeed = 0;
      }
    }
  }

  if (shared->servo.stepsToGo == 0) {
    // Security measure, end of travel
    shared->servo.currentSpeed = 0;
  } else {
    int32_t positionIncrement = ((int32_t)((float)shared->servo.currentSpeed * (float)shared->executionInterval + (float)data->rampsDeltaPos.error) / 100000000);
    data->rampsDeltaPos.error = ((int32_t)((float)shared->servo.currentSpeed * (float)shared->executionInterval + (float)data->rampsDeltaPos.error) % 100000000);
    shared->servo.desiredSteps += positionIncrement;
    shared->servo.stepsToGo -= positionIncrement;
    shared->fastData.stepsToGo = shared->servo.stepsToGo;
  }
}

static inline void updateJogPosition(rampsHandler_t *data) {
  rampsSharedData_t *shared = &(data->shared);
  float interval = (float) shared->executionInterval / 100000000.0f;

  // Start Pos
  if (shared->servo.jogSpeed > 0) {
    if (shared->servo.currentSpeed < shared->servo.jogSpeed) {
      shared->servo.currentSpeed += shared->servo.acceleration * interval;
      // max speed
      if (shared->servo.currentSpeed > shared->servo.jogSpeed) {
        shared->servo.currentSpeed = shared->servo.jogSpeed;
      }
    }
  }

  // Start Neg
  if (shared->servo.jogSpeed < 0) {
    if (shared->servo.currentSpeed > shared->servo.jogSpeed) {
      shared->servo.currentSpeed -= shared->servo.acceleration * interval;
      if (shared->servo.currentSpeed < shared->servo.jogSpeed) {
        shared->servo.currentSpeed = shared->servo.jogSpeed;
      }
    }
  }

  // Stop Pos/Neg
  if (shared->servo.currentSpeed > 0) {
    if (shared->servo.currentSpeed > shared->servo.jogSpeed) {
      shared->servo.currentSpeed -= shared->servo.acceleration * interval;
      if (shared->servo.currentSpeed < 0) shared->servo.currentSpeed = 0;
    }
  }

  if (shared->servo.currentSpeed < 0) {
    if (shared->servo.currentSpeed < shared->servo.jogSpeed) {
      shared->servo.currentSpeed += shared->servo.acceleration * interval;
      if (shared->servo.currentSpeed > 0) shared->servo.currentSpeed = 0;
    }
  }

  int32_t positionIncrement = ((int32_t)((float)shared->servo.currentSpeed * (float)shared->executionInterval + (float)data->rampsDeltaPos.error) / 100000000);
  data->rampsDeltaPos.error = ((int32_t)((float)shared->servo.currentSpeed * (float)shared->executionInterval + (float)data->rampsDeltaPos.error) % 100000000);
  shared->servo.desiredSteps += positionIncrement;
}

RAM_FUNC void SynchroRefreshTimerIsr(rampsHandler_t *data) {
  uint32_t start = DWT->CYCCNT;
  // Reset the step pin as soon as possible
  gpioReset(STEP_GPIO_PORT, STEP_PIN);
  rampsSharedData_t *shared = &(data->shared);
  shared->executionIntervalPrevious = shared->executionIntervalCurrent;
  shared->executionIntervalCurrent = DWT->CYCCNT;
  shared->executionInterval = shared->executionIntervalCurrent - shared->executionIntervalPrevious;
  shared->fastData.executionInterval = shared->executionInterval;

  for (int i = 0; i < SCALES_COUNT; i++) {
    data->scalesDeltaPos[i].oldPosition = data->scalesDeltaPos[i].position;
    data->scalesDeltaPos[i].position = __HAL_TIM_GET_COUNTER(data->shared.scales[i].timerHandle);
    data->scalesDeltaPos[i].delta = (int16_t) (data->scalesDeltaPos[i].position - data->scalesDeltaPos[i].oldPosition);
    /* user-level direction flip (the board-level inversion is fixed in Scales.c) */
    if (shared->scales[i].dirInvert) data->scalesDeltaPos[i].delta = -data->scalesDeltaPos[i].delta;
    shared->scales[i].position += data->scalesDeltaPos[i].delta;

    // calculate delta for sync ratio configured for the current scale
    deltaPositionAndError(
      shared->scales[i].position,
      shared->scales[i].syncRatioNum,
      shared->scales[i].syncRatioDen,
      &data->scalesSyncDeltaPos[i]
    );

    // request motion only if sync is enabled
    if (shared->scales[i].syncEnable != 0) {
      shared->servo.desiredSteps += data->scalesSyncDeltaPos[i].scaledDelta;
    }

    // Update fastData current position
    shared->fastData.scaleCurrent[i] = shared->scales[i].position;
  }

  if (shared->fastData.servoMode == 1) updateIndexingPosition(data);
  if (shared->fastData.servoMode == 2) updateJogPosition(data);

  if (shared->fastData.servoMode != 0 && servoCyclesCounter == 0) {
    int32_t change = (int32_t)(shared->servo.desiredSteps) - (int32_t)shared->servo.currentSteps;
    // generate pulses to reach desired position with the motor
    uint32_t direction = 1;

    if (change > 0) {
      direction = 1;
      gpioSet(DIR_GPIO_PORT, DIR_PIN);
    }
    if (change < 0) {
      gpioReset(DIR_GPIO_PORT, DIR_PIN);
      direction = -1;
    }

    if (direction == data->servoPreviousDirection && change != 0) {
      gpioSet(STEP_GPIO_PORT, STEP_PIN);
      shared->servo.currentSteps += direction;
    }

    data->servoPreviousDirection = direction;
  }

  servoCyclesCounter = (servoCyclesCounter + 1) % servoCycles;

  shared->executionCycles = DWT->CYCCNT - start;
  if (shared->executionCycles > shared->diag.cyclesMax)   /* worst-case hold (diag.cycmax) */
    shared->diag.cyclesMax = shared->executionCycles;
}

/* Repeating diagnostic heartbeat: gBlinkCode short blinks, then a gap, ~once per second
 * (BlinkCode.h). Default BLINK_APP (1) = running normally; the net task raises
 * BLINK_NET_DOWN/BLINK_NET_ERROR while the W5500 link is unhappy. */
volatile uint8_t gBlinkCode = BLINK_APP;

_Noreturn void userLedTask(__attribute__((unused)) void *argument) {
  for (;;) {
    uint8_t code = gBlinkCode ? gBlinkCode : 1;
    for (uint8_t i = 0; i < code; i++) {
      gpioReset(USR_LED_GPIO_Port, USR_LED_Pin);   /* active-low: LOW = LED on  */
      osDelay(BLINK_ON_MS);
      gpioSet(USR_LED_GPIO_Port, USR_LED_Pin);     /* HIGH = LED off            */
      osDelay(BLINK_OFF_MS);
    }
    osDelay(BLINK_GAP_MS);                          /* gap (LED off) before repeat */
  }
}

const int32_t updateSpeedTaskTicks = 50;

_Noreturn void updateSpeedTask(void *argument) {
  rampsHandler_t *rampsData = (rampsHandler_t *) argument;

  for (;;) {

    // Update the current speed
    osDelay(updateSpeedTaskTicks);

    // Update fast access variables
    rampsData->shared.fastData.cycles = rampsData->shared.executionCycles;
    rampsData->shared.fastData.servoCurrent = rampsData->shared.servo.currentSteps;
    rampsData->shared.fastData.servoDesired = rampsData->shared.servo.desiredSteps;
    rampsData->shared.diag.uptimeS = osKernelGetTickCount() / 1000U;

    // If maximum speed has been changed, update the motor timer accordingly
    float clock_freq = 100000000.0f / ((float) rampsData->synchroRefreshTimer->Init.Prescaler + 1) /
                       (float) (rampsData->synchroRefreshTimer->Init.Period + 1);

    // Clamping value for max speed to the maximum allowed by the current timer refresh rate from the sync routine
    if (rampsData->shared.servo.maxSpeed > 100000) {
      rampsData->shared.servo.maxSpeed = 100000;
    }

    float newPeriod = floorf(clock_freq / rampsData->shared.servo.maxSpeed);
    if (newPeriod > (float) UINT16_MAX) {
      newPeriod = 65535;
    }
    if (newPeriod < 1) {
      newPeriod = 0;
    }
    servoCycles = (uint16_t) newPeriod;

    for (int i = 0; i < SCALES_COUNT; i++) {
      // Update scale/spindle speed value
      deltaPositionAndError(
        rampsData->shared.scales[i].position,
        updateSpeedTaskTicks,
        HAL_GetTickFreq(),
        &rampsData->scalesSpeed[i]
      );
      rampsData->shared.scales[i].speed = rampsData->scalesSpeed[i].scaledDelta;
      rampsData->shared.fastData.scaleSpeed[i] = rampsData->scalesSpeed[i].scaledDelta;
    }
  }
}

_Noreturn void servoEnableTask(void *argument) {
  rampsHandler_t *rampsData = (rampsHandler_t *) argument;
  rampsSharedData_t *shared = (rampsSharedData_t *) &rampsData->shared;
  uint32_t previousPosition = 0;

  for (;;) {
    osDelay(100);

    bool anySyncMotionEnabled = false;
    for (int i = 0; i < SCALES_COUNT; i++) {
      anySyncMotionEnabled = anySyncMotionEnabled || (shared->scales[i].syncEnable != 0);
    }

    if (anySyncMotionEnabled && rampsData->shared.fastData.servoMode != 2)
      rampsData->shared.fastData.servoMode = 1;

    rampsData->shared.fastData.servoSpeed = (float)(int32_t)(rampsData->shared.servo.currentSteps - previousPosition) * 10;
    previousPosition = rampsData->shared.servo.currentSteps;

    if (shared->fastData.servoMode != 0) gpioReset(ENA_GPIO_PORT, ENA_PIN);
    if (shared->fastData.servoMode == 0) gpioSet(ENA_GPIO_PORT, ENA_PIN);
  }
}
