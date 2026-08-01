/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Bootloader USR_LED heartbeat (PB12). Implemented in main.c. bl_led_service() is a
 * non-blocking, SysTick-driven state machine: call it frequently from the CLI poll loop
 * and it renders the current blink code as a repeating pattern (BlinkCode.h).
 */
#ifndef BL_LED_H
#define BL_LED_H

#include <stdint.h>

void bl_led_set(uint8_t code);   /* set the repeating blink code (BlinkCode.h) */
void bl_led_service(void);       /* advance the pattern; call often (polled) */

#endif /* BL_LED_H */
