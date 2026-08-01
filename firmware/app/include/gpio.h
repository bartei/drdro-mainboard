#ifndef __GPIO_H__
#define __GPIO_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

void MX_GPIO_Init(void);

/* Status LED helpers — hide the active-low wiring (see BoardPins.h). */
void UsrLedSet(int on);
void UsrLedToggle(void);

#ifdef __cplusplus
}
#endif
#endif /*__ GPIO_H__ */
