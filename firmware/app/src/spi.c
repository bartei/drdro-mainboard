#include "spi.h"

SPI_HandleTypeDef hspi2;

/**
 * SPI2 master for the W5500 (U19).
 *
 * - NSS is SOFT: the W5500 requires SCSn to be held low for a whole
 *   variable-length-data-mode transaction (address + control + N data bytes).
 *   Hardware NSS would toggle per frame and break the protocol. CS is driven as
 *   a plain GPIO in Net.c.
 * - Mode 0 (CPOL=LOW, CPHA=1EDGE). The W5500 supports SPI modes 0 and 3.
 * - SPI2 is on APB1 = 50 MHz. /8 => 6.25 MHz SCK: comfortably inside the
 *   W5500's rating and conservative for a first bring-up. It can be raised once
 *   the link is proven.
 */
void MX_SPI2_Init(void)
{
  hspi2.Instance               = W5500_SPI;
  hspi2.Init.Mode              = SPI_MODE_MASTER;
  hspi2.Init.Direction         = SPI_DIRECTION_2LINES;
  hspi2.Init.DataSize          = SPI_DATASIZE_8BIT;
  hspi2.Init.CLKPolarity       = SPI_POLARITY_LOW;
  hspi2.Init.CLKPhase          = SPI_PHASE_1EDGE;
  hspi2.Init.NSS               = SPI_NSS_SOFT;
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
  hspi2.Init.FirstBit          = SPI_FIRSTBIT_MSB;
  hspi2.Init.TIMode            = SPI_TIMODE_DISABLE;
  hspi2.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
  hspi2.Init.CRCPolynomial     = 10;
  if (HAL_SPI_Init(&hspi2) != HAL_OK)
  {
    Error_Handler();
  }
}

void HAL_SPI_MspInit(SPI_HandleTypeDef *spiHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if (spiHandle->Instance == W5500_SPI)
  {
    __HAL_RCC_SPI2_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /**SPI2 GPIO Configuration
    PB13     ------> SPI2_SCK
    PB14     ------> SPI2_MISO
    PB15     ------> SPI2_MOSI
    (PB12 = SCSn is a GPIO, configured in Net.c — deliberately not an AF pin)
    */
    GPIO_InitStruct.Pin       = W5500_SCK_Pin | W5500_MISO_Pin | W5500_MOSI_Pin;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_NOPULL;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = W5500_SPI_AF;
    HAL_GPIO_Init(W5500_SPI_GPIO_Port, &GPIO_InitStruct);
  }
}

void HAL_SPI_MspDeInit(SPI_HandleTypeDef *spiHandle)
{
  if (spiHandle->Instance == W5500_SPI)
  {
    __HAL_RCC_SPI2_CLK_DISABLE();
    HAL_GPIO_DeInit(W5500_SPI_GPIO_Port,
                    W5500_SCK_Pin | W5500_MISO_Pin | W5500_MOSI_Pin);
  }
}
