/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Motion core + shared register image — ported from the drdro-firmware-f4
 * baseline to the V1.5 mainboard. Pin assignments come from shared/BoardPins.h
 * (M1 STEP/DIR + shared ENA through the ULN2003; the baseline's SPARE pins do
 * not exist here — their pins are encoder inputs on this board).
 *
 * The nested rampsSharedData_t is the protocol register image: Protocol.c
 * addresses fields by offsetof, so the field layout is part of the host
 * contract. Extend append-style and update the kVars registry together.
 */
#ifndef DRDRO_RAMPS_H_
#define DRDRO_RAMPS_H_

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx_hal.h"
#include "cmsis_os2.h"
#include "BoardPins.h"
#include "Scales.h"
#include "BlinkCode.h"

/* USR_LED diagnostic code rendered by userLedTask (BlinkCode.h). Default BLINK_APP. */
extern volatile uint8_t gBlinkCode;

/* Place a function in RAM (.RamFunc — loaded in flash, copied to RAM by startup) so it
 * can execute while flash is being erased/programmed (single-bank flash stalls all flash
 * reads). Used for the motion ISR path so a settings/bank save never drops steps. */
#define RAM_FUNC __attribute__((section(".RamFunc")))

/* Fast GPIO writes — a single store to BSRR, far cheaper than HAL_GPIO_WritePin and,
 * being inline (no flash call), safe on the motion ISR hot path / in RAM-resident code. */
static inline void gpioSet(GPIO_TypeDef *port, uint16_t pin)   { port->BSRR = pin; }
static inline void gpioReset(GPIO_TypeDef *port, uint16_t pin) { port->BSRR = (uint32_t)pin << 16; }

/* Motion pins (BoardPins.h): the motion core drives M1 only; M2/M3 are configured
 * as idle outputs for bring-up via the `dout` command. All on GPIOC. */
#define STEP_PIN        M1_STEP_Pin
#define STEP_GPIO_PORT  MOTOR_GPIO_Port
#define DIR_PIN         M1_DIR_Pin
#define DIR_GPIO_PORT   MOTOR_GPIO_Port
#define ENA_PIN         M_ENA_Pin
#define ENA_GPIO_PORT   MOTOR_GPIO_Port

typedef struct {
  int32_t delta;
  uint32_t oldPosition;
  uint32_t position;
  int32_t scaledDelta;
  int32_t error;
} deltaPosError_t;

typedef struct {
  TIM_HandleTypeDef *timerHandle;
  int32_t position;
  int32_t speed;
  int32_t syncRatioNum, syncRatioDen;
  uint16_t syncEnable;
  uint16_t filterValue;   /* encoder input-capture filter, 0..SCALES_FILTER_MAX */
  uint16_t dirInvert;     /* user-level count direction flip (0|1); the board-level
                             AM26LV32E inversion is already fixed in Scales.c */
  uint16_t pad0;          /* keep 32-bit alignment of the next element */
} input_t;

typedef struct {
  float maxSpeed;
  float currentSpeed;
  float jogSpeed;
  float acceleration;
  float indexSpeed;     /* feedrate cap for indexing/offset moves; 0 = use maxSpeed */
  int32_t stepsToGo;
  uint32_t destinationSteps;
  uint32_t currentSteps;
  uint32_t desiredSteps;
} servo_t;

typedef struct {
  uint32_t servoCurrent;
  uint32_t servoDesired;
  uint32_t stepsToGo;
  float servoSpeed;
  int32_t scaleCurrent[SCALES_COUNT];
  int32_t scaleSpeed[SCALES_COUNT];
  uint32_t cycles;
  uint32_t executionInterval;
  uint16_t servoMode; // Servo modes: 0=disabled, 1=sync/index, 2=jog
} fastData_t;

/* Digital (opto) inputs — written by the din poll task, read-only to the host. */
typedef struct {
  uint16_t state;                    /* ISO_IN1..6 bitmask, 1 = active (input pulled low) */
  uint16_t debounceMs;               /* debounce window, persisted */
  uint32_t count[6];                 /* rising-edge counters */
} dinData_t;

/* Analog outputs — GP8403 dual 12-bit DAC, 0..4095 => 0..10 V. */
typedef struct {
  uint16_t raw[2];
} aoutData_t;

/* Network status/config mirror for the registry. Live fields are refreshed by
 * the net task; cfg fields are persisted and applied at the next boot. */
typedef struct {
  uint8_t  ip[4], mask[4], gw[4];    /* live (lease or static), RO */
  uint8_t  mac[6];                   /* live, RO */
  uint16_t state;                    /* NetState_t, RO */
  uint16_t dhcp;                     /* RW: 1 = DHCP, 0 = static (next boot) */
  uint16_t port;                     /* RW: TCP CLI port (next boot) */
  uint16_t pad0;
  uint8_t  cfgIp[4], cfgMask[4], cfgGw[4];   /* RW: static config (next boot) */
} netData_t;

/* Runtime statistics for the registry (diag.*). Cycle counts are DWT ticks at
 * 100 MHz (10 ns/tick); the motion ISR budget is 1000 ticks (10 µs). */
typedef struct {
  uint32_t uptimeS;     /* seconds since boot (updateSpeedTask, RO) */
  uint32_t cyclesMax;   /* max-hold ISR duration since boot/reset — the worst
                           case against the budget. Write 0 to re-arm. */
} diagData_t;

typedef struct {
  uint32_t executionInterval;
  uint32_t executionIntervalPrevious;
  uint32_t executionIntervalCurrent;
  uint32_t executionCycles;
  servo_t servo;
  input_t scales[SCALES_COUNT];
  fastData_t fastData;
  dinData_t din;
  aoutData_t aout;
  netData_t net;
  diagData_t diag;
} rampsSharedData_t;

typedef struct {
  // Comm shared data (protocol register image)
  rampsSharedData_t shared;

  // STM32 Related
  TIM_HandleTypeDef *synchroRefreshTimer;
  UART_HandleTypeDef *commUart;

  deltaPosError_t scalesDeltaPos[SCALES_COUNT];
  deltaPosError_t scalesSyncDeltaPos[SCALES_COUNT];
  deltaPosError_t scalesSpeed[SCALES_COUNT];
  deltaPosError_t rampsDeltaPos;
  uint32_t servoPreviousDirection;
} rampsHandler_t;

void RampsStart(rampsHandler_t *rampsData);

RAM_FUNC void SynchroRefreshTimerIsr(rampsHandler_t *data);

_Noreturn void updateSpeedTask(void *argument);

_Noreturn void userLedTask(__attribute__((unused)) void *argument);

_Noreturn void servoEnableTask(void *argument);

#endif /* DRDRO_RAMPS_H_ */
