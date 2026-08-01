/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * GP8403 analog-output driver — see Aout.h. Kept deliberately tiny and
 * isolated: if the register map turns out to differ on the bench, the fix is
 * one function. Blocking I2C in caller context (CLI task / early boot).
 */
#include "Aout.h"
#include "i2c.h"

#define GP8403_REG_CONFIG   0x01U
#define GP8403_CFG_0_10V    0x11U   /* output range 0-10 V (0x00 = 0-5 V) */
#define GP8403_REG_VOUT0    0x02U
#define GP8403_REG_VOUT1    0x04U
#define GP8403_TIMEOUT_MS   50U

static int sPresent = 0;   /* config write succeeded at boot */

int AoutInit(void)
{
  uint8_t cfg = GP8403_CFG_0_10V;
  HAL_StatusTypeDef rc = HAL_I2C_Mem_Write(&hi2c1, GP8403_I2C_ADDR, GP8403_REG_CONFIG,
                                           I2C_MEMADD_SIZE_8BIT, &cfg, 1U, GP8403_TIMEOUT_MS);
  sPresent = (rc == HAL_OK);
  return sPresent ? 0 : -1;
}

int AoutWrite(uint8_t ch, uint16_t raw)
{
  if (ch > 1U) return -1;
  if (raw > 4095U) raw = 4095U;
  /* 12-bit code, left-justified in 16 bits, low byte first on the wire. */
  uint8_t data[2] = {
    (uint8_t)((raw << 4) & 0xF0U),
    (uint8_t)(raw >> 4),
  };
  uint8_t reg = ch ? GP8403_REG_VOUT1 : GP8403_REG_VOUT0;
  HAL_StatusTypeDef rc = HAL_I2C_Mem_Write(&hi2c1, GP8403_I2C_ADDR, reg,
                                           I2C_MEMADD_SIZE_8BIT, data, 2U, GP8403_TIMEOUT_MS);
  return (rc == HAL_OK) ? 0 : -1;
}

void AoutApply(const rampsSharedData_t *shared)
{
  if (!sPresent) return;
  for (uint8_t ch = 0; ch < 2U; ch++)
    AoutWrite(ch, shared->aout.raw[ch]);
}
