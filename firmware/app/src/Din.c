/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Digital input poll/debounce task — see Din.h.
 */
#include "Din.h"
#include "cmsis_os2.h"

typedef struct { GPIO_TypeDef *port; uint16_t pin; } din_pin_t;
static const din_pin_t kPins[6] = {
  { ISO_IN1_GPIO_Port, ISO_IN1_Pin },
  { ISO_IN2_GPIO_Port, ISO_IN2_Pin },
  { ISO_IN3_GPIO_Port, ISO_IN3_Pin },
  { ISO_IN4_GPIO_Port, ISO_IN4_Pin },
  { ISO_IN5_GPIO_Port, ISO_IN5_Pin },
  { ISO_IN6_GPIO_Port, ISO_IN6_Pin },
};

static rampsSharedData_t *sShared = NULL;

static const osThreadAttr_t kDinTaskAttr = {
  .name = "din", .stack_size = 128 * 4, .priority = (osPriority_t) osPriorityLow,
};

/* 1 ms sampling with a per-pin integrator: a raw level must persist for
 * debounceMs consecutive samples before it becomes the stable state. Rising
 * edges of the STABLE state bump the counters. */
static _Noreturn void dinTask(void *arg)
{
  (void)arg;
  uint8_t integ[6] = {0};
  uint16_t stable = 0;                    /* debounced bitmask */

  for (;;) {
    osDelay(1);
    uint16_t window = sShared->din.debounceMs;
    if (window < 1U) window = 1U;
    if (window > 250U) window = 250U;

    uint16_t newStable = stable;
    for (int i = 0; i < 6; i++) {
      /* TLP2309 sinks when active: pin LOW = input active = bit set. */
      uint16_t raw = (HAL_GPIO_ReadPin(kPins[i].port, kPins[i].pin) == GPIO_PIN_RESET) ? 1U : 0U;
      uint16_t cur = (stable >> i) & 1U;
      if (raw == cur) {
        integ[i] = 0;
      } else if (++integ[i] >= (uint8_t)window) {
        integ[i] = 0;
        if (raw) {
          newStable |= (uint16_t)(1U << i);
          sShared->din.count[i]++;        /* rising edge of the debounced state */
        } else {
          newStable &= (uint16_t)~(1U << i);
        }
      }
    }
    stable = newStable;
    sShared->din.state = stable;
  }
}

void DinStart(rampsSharedData_t *shared)
{
  sShared = shared;

  GPIO_InitTypeDef g = {0};
  g.Mode = GPIO_MODE_INPUT;
  g.Pull = GPIO_PULLUP;                   /* open-collector sources — mandatory */
  for (int i = 0; i < 6; i++) {
    g.Pin = kPins[i].pin;
    HAL_GPIO_Init(kPins[i].port, &g);
  }

  osThreadNew(dinTask, NULL, &kDinTaskAttr);
}
