/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * USR_LED (PA12, active-low) diagnostic blink codes, shared by the app and the
 * bootloader. The LED repeats a pattern of N short blinks followed by a ~0.7 s gap
 * so the current mode (or error) can be read off visually. App and bootloader
 * render this with their own timing source (FreeRTOS task / polled SysTick), but
 * agree on the code values.
 *
 * Codes 3/4 absorb the network states the V1.5 bring-up image used to signal with
 * ad-hoc duty cycles: the W5500 driver (Net.c) sets the code, the LED task renders
 * it. Fine-grained state remains readable via the `net.state` registry variable.
 */
#ifndef BLINKCODE_H
#define BLINKCODE_H

typedef enum {
  BLINK_APP        = 1,   /* application running normally (network up or disabled) */
  BLINK_BOOTLOADER = 2,   /* bootloader CLI / update mode */
  BLINK_NET_DOWN   = 3,   /* no link / DHCP in progress / DHCP failed */
  BLINK_NET_ERROR  = 4,   /* W5500 not responding (chip error) */
  BLINK_ERR_FLASH  = 5,   /* flash erase/program/verify failure */
} blink_code_t;

/* Pattern timing (ms): each blink = ON then OFF, then a gap before repeating. */
#define BLINK_ON_MS   120U
#define BLINK_OFF_MS  160U
#define BLINK_GAP_MS  700U

#endif /* BLINKCODE_H */
