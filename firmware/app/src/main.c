/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * drDRO mainboard V1.5 application — entry point and clock tree.
 * Runs from the Exec region (0x08020000) under the IAP bootloader in sector 0;
 * see shared/Bootloader.h for the flash map and the handoff contract.
 */
#include "main.h"
#include "cmsis_os.h"
#include "gpio.h"
#include "tim.h"
#include "usart.h"
#include "i2c.h"
#include "Net.h"
#include "NetCli.h"
#include "Ramps.h"
#include "Protocol.h"
#include "SettingsStore.h"
#include "Din.h"
#include "Aout.h"
#include "FwUpdate.h"
#include "Bootloader.h"   /* APP_EXEC_BASE — keep VTOR in sync with the linker/bootloader */

rampsHandler_t RampsData;

/* Vector table relocated to RAM so interrupt entry (esp. the motion ISR) doesn't fetch
 * from flash — lets the step ISR keep running while flash is erased/programmed (settings
 * /bank save, Ethernet update into a bank). 512-byte aligned (VTOR requires alignment
 * >= table size, ~101 vectors). */
static uint32_t g_ramVectors[128] __attribute__((aligned(0x200)));

void SystemClock_Config(void);

/* Hand off to the IAP bootloader by JUMPING to 0x08000000 — same mechanism as the
 * proven baseline. (BOOT0 is pulled down on this board so a reset would also be
 * safe, but the jump preserves identical host-observed behavior and the no-init
 * BOOT_FLAG.) Tears down the running app (mask IRQs, stop SysTick, clear NVIC,
 * switch to MSP) then branches to the bootloader's reset vector. */
void EnterBootloader(void) {
  __disable_irq();
  SysTick->CTRL = 0U; SysTick->LOAD = 0U; SysTick->VAL = 0U;
  for (int i = 0; i < 8; i++) { NVIC->ICER[i] = 0xFFFFFFFFU; NVIC->ICPR[i] = 0xFFFFFFFFU; }
  uint32_t sp = *(volatile uint32_t *)BL_BASE_ADDR;
  uint32_t pc = *(volatile uint32_t *)(BL_BASE_ADDR + 4U);
  __set_CONTROL(0);                 /* leave the FreeRTOS task PSP — run on MSP */
  __ISB();
  SCB->VTOR = BL_BASE_ADDR;
  __set_MSP(sp);
  __DSB(); __ISB();
  __enable_irq();                   /* bootloader needs SysTick/flash IRQs; none pending */
  ((void (*)(void))pc)();
  while (1) { }                     /* not reached */
}

/* ------------------------------------------------------------------------- */
/* Clock-tree constraints, enforced at compile time.                         */
/*                                                                           */
/* Getting HSE_VALUE or PLLM wrong is silent at build time but breaks every   */
/* derived clock (UART baud, SPI SCK, timer rates) — and this project inherits */
/* its clock code from a board with a DIFFERENT crystal, so the mistake is a  */
/* realistic one. These assertions make it a build error instead.            */
/* Limits are from RM0383 (PLL) and DS10314 (max frequencies).               */
/* ------------------------------------------------------------------------- */
#define CFG_PLLM        8U
#define CFG_PLLN        100U
#define CFG_PLLP        2U
#define CFG_PLL_VCO_IN  (HSE_VALUE / CFG_PLLM)
#define CFG_PLL_VCO_OUT (CFG_PLL_VCO_IN * CFG_PLLN)
#define CFG_SYSCLK      (CFG_PLL_VCO_OUT / CFG_PLLP)

_Static_assert(HSE_VALUE == 16000000U,
               "Board crystal X1 is 16 MHz (X322516MLB4SI) - check -D HSE_VALUE");
_Static_assert(CFG_PLL_VCO_IN >= 1000000U && CFG_PLL_VCO_IN <= 2000000U,
               "PLL input (HSE/PLLM) must be 1-2 MHz, 2 MHz recommended");
_Static_assert(CFG_PLL_VCO_OUT >= 100000000U && CFG_PLL_VCO_OUT <= 432000000U,
               "PLL VCO output must be 100-432 MHz");
_Static_assert(CFG_SYSCLK == 100000000U,
               "SYSCLK must be 100 MHz (STM32F411 maximum)");

/**
  * @brief  The application entry point.
  */
int main(void)
{
  /* IAP: this image runs from the Exec region (0x08020000). Copy the vector table into
   * RAM and point VTOR at it (before HAL_Init, so the HAL/NVIC use it): keeping vectors
   * in RAM means interrupt entry never fetches from flash, so the motion ISR keeps firing
   * during a flash erase/program (settings/bank save/network update). */
  for (uint32_t i = 0; i < 128U; i++)
    g_ramVectors[i] = ((const volatile uint32_t *)APP_EXEC_BASE)[i];
  SCB->VTOR = (uint32_t)g_ramVectors;
  __DSB();
  __ISB();

  /* The IAP bootloader hands off with a JUMP and PRIMASK set (bl_jump_to_exec
   * does __disable_irq() and never re-enables). Without this, the whole of
   * main() runs with the HAL tick frozen — every HAL timeout is infinite, so a
   * misbehaving peripheral (e.g. a hung I2C bus in AoutInit) wedges the boot —
   * until osKernelStart()'s cpsie finally unmasks interrupts. On a clean
   * power-on reset PRIMASK is already clear and this is a no-op. */
  __enable_irq();

  /* Reset of all peripherals, initializes the Flash interface and the Systick. */
  HAL_Init();

  /* Configure the system clock */
  SystemClock_Config();

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_TIM1_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_TIM4_Init();
  MX_TIM5_Init();
  MX_USART1_UART_Init();
  MX_I2C1_Init();
  MX_TIM9_Init();
  // htim1..htim5 count the five encoder scales (quadrature)
  // htim9 generates the 100 kHz synchro motion interrupt
  // i2c1 carries the GP8403 analog-out DAC (+ the DB9 expansion bus)

  DebugPrint("\r\ndrDRO mainboard V1.5 (STM32F411RET6) " FW_VERSION "\r\n");

  RampsData.shared.scales[0].timerHandle = &htim1;
  RampsData.shared.scales[1].timerHandle = &htim2;
  RampsData.shared.scales[2].timerHandle = &htim3;
  RampsData.shared.scales[3].timerHandle = &htim4;
  RampsData.shared.scales[4].timerHandle = &htim5;
  RampsData.synchroRefreshTimer = &htim9;
  RampsData.commUart = &huart1;
  RampsStart(&RampsData);

  /* Override compiled defaults with persisted settings (scales ratios, servo cfg,
   * aout/net/din config) if a valid image is present in flash. Before the
   * scheduler starts. */
  SettingsApply(&RampsData.shared);

  /* The encoder timers were started by RampsStart with the compiled-in filter;
   * reprogram them with the persisted per-scale value. */
  for (int i = 0; i < SCALES_COUNT; i++)
    setScaleFilter(RampsData.shared.scales[i].timerHandle, RampsData.shared.scales[i].filterValue);

  /* Digital inputs: pull-ups + debounce poll task. The task ALSO brings up the
   * GP8403 analog outputs — deliberately NOT done here: from the first
   * osThreadNew above until osKernelStart, FreeRTOS holds BASEPRI at the
   * syscall mask, so the TIM11 HAL tick is frozen and any blocking HAL call
   * (I2C!) has an infinite timeout. A hung DAC here = a bricked boot. */
  DinStart(&RampsData.shared);

  /* Init the RTOS. (Tasks were created in RampsStart — same order as the proven
   * baseline: xTaskCreate is legal before the scheduler starts.) */
  osKernelInitialize();

  /* SPI2 + the W5500 are brought up inside the net task (it needs osDelay for
   * the reset timing and blocks on DHCP). Reports state via gBlinkCode and the
   * net.* registry variables; honors the persisted static-IP config. */
  NetStart(&RampsData.shared);

  /* TCP CLI: the same line protocol as RS-485, on port net.port (default 5555). */
  NetCliStart(&RampsData.shared);

  /* Network firmware update: fw.* commands stream an image into the inactive
   * bank on port net.port+1; the bootloader activates it on the next reset. */
  FwUpdateStart(&RampsData.shared);

  osKernelStart();

  /* Control is now with the scheduler; never reached. */
  while (1)
  {
  }
}

/**
  * @brief System Clock Configuration
  *
  * HSE = 16 MHz crystal (X1 = X322516MLB4SI, per the board netlist).
  *
  *   PLL input   = HSE / PLLM = 16 MHz / 8   = 2 MHz   (VCO input must be 1-2 MHz)
  *   VCO         = 2 MHz * PLLN = 2 * 100    = 200 MHz
  *   SYSCLK      = VCO / PLLP = 200 / 2      = 100 MHz (F411 maximum)
  *   PLLQ output = VCO / 4                   = 50 MHz  (USB/SDIO — unused here)
  *
  * NOTE this differs from the F4 baseline project, which has an 8 MHz crystal and
  * therefore uses PLLM = 4. Keeping PLLM = 4 here would give a 4 MHz VCO input
  * and a 200 MHz SYSCLK request — out of spec and it would not run.
  *
  * AHB = 100 MHz, APB1 = 50 MHz (SPI2), APB2 = 100 MHz (USART1, TIM9).
  * Flash latency 3 WS is correct for 100 MHz at 3.3 V.
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = CFG_PLLM;   /* 8 for the 16 MHz HSE (was 4 @ 8 MHz) */
  RCC_OscInitStruct.PLL.PLLN = CFG_PLLN;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;   /* == CFG_PLLP */
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief  Period elapsed callback — TIM11 drives the HAL timebase (SysTick
  *         belongs to FreeRTOS; SysTick_Handler lives in cmsis_os2.c).
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM11) {
    HAL_IncTick();
  }
}

/**
  * @brief  This function is executed in case of error occurrence.
  */
void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}

#ifdef  USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
}
#endif /* USE_FULL_ASSERT */
