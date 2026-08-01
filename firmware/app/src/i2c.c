/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * I2C1 — the board's expansion bus (also exported on DB9 pins 5/9 of all five
 * encoder connectors) carrying the GP8403 analog-output DAC at 0x58.
 * PB8 = SCL, PB9 = SDA, AF4, external 4.7k pull-ups (R74/R14). 100 kHz standard
 * mode: the bus leaves the board, so keep the conservative speed.
 */
#include "i2c.h"

I2C_HandleTypeDef hi2c1;

void MX_I2C1_Init(void)
{
  hi2c1.Instance             = I2C1;
  hi2c1.Init.ClockSpeed      = 100000;
  hi2c1.Init.DutyCycle       = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1     = 0;
  hi2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2     = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
}

void HAL_I2C_MspInit(I2C_HandleTypeDef* i2cHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if (i2cHandle->Instance == I2C1)
  {
    __HAL_RCC_GPIOB_CLK_ENABLE();
    /* PB8 = I2C1_SCL, PB9 = I2C1_SDA — open-drain, external pull-ups */
    GPIO_InitStruct.Pin       = I2C1_SCL_Pin | I2C1_SDA_Pin;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Pull      = GPIO_NOPULL;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = I2C1_AF;
    HAL_GPIO_Init(I2C1_GPIO_Port, &GPIO_InitStruct);

    __HAL_RCC_I2C1_CLK_ENABLE();
  }
}

void HAL_I2C_MspDeInit(I2C_HandleTypeDef* i2cHandle)
{
  if (i2cHandle->Instance == I2C1)
  {
    __HAL_RCC_I2C1_CLK_DISABLE();
    HAL_GPIO_DeInit(I2C1_GPIO_Port, I2C1_SCL_Pin | I2C1_SDA_Pin);
  }
}
