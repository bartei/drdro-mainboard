/**
 * Net.h — W5500 Ethernet bring-up (DHCP) for the drDRO mainboard V1.5.
 *
 * The W5500 (U19) carries the whole TCP/IP stack in silicon; the MCU only talks
 * to it over SPI2. This module owns:
 *   - the SPI/CS/critical-section glue the WIZnet ioLibrary calls back into,
 *   - the hardware reset + identity check,
 *   - a DHCP client task that keeps the lease renewed.
 */
#ifndef __NET_H
#define __NET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "Ramps.h"   /* rampsSharedData_t — net status/config mirror */

typedef enum {
  NET_STATE_INIT = 0,    /**< before/while the W5500 is being brought up      */
  NET_STATE_CHIP_ERROR,  /**< SPI reachable? VERSIONR mismatch -> wiring/SPI  */
  NET_STATE_NO_LINK,     /**< chip alive, Ethernet link down (cable/switch)   */
  NET_STATE_DHCP_WAIT,   /**< link up, DISCOVER/REQUEST in flight             */
  NET_STATE_DHCP_FAILED, /**< no server answered within the retry budget      */
  NET_STATE_LEASED       /**< address acquired and held                       */
} NetState_t;

/** Current bring-up state (drives the status-LED pattern). */
NetState_t NetGetState(void);

/** Last acquired address info, valid once NetGetState() == NET_STATE_LEASED. */
void NetGetAddress(uint8_t ip[4], uint8_t sn[4], uint8_t gw[4], uint8_t dns[4]);

/** MAC address in use (derived from the STM32 unique device ID). */
void NetGetMac(uint8_t mac[6]);

/** Create the DHCP/link-maintenance task. Call before starting the scheduler,
 *  after SettingsApply (the task honors shared->net.dhcp / cfg* for static IP).
 *  Live status (state/mac/ip/mask/gw) is mirrored into shared->net for the
 *  protocol registry. */
void NetStart(rampsSharedData_t *shared);

#ifdef __cplusplus
}
#endif

#endif /* __NET_H */
