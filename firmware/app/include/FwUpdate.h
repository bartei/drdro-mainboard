/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Firmware update over Ethernet — app-side bank writer.
 *
 * The dual-bank IAP design makes network updates an APP feature: the running
 * app streams the new image into the INACTIVE storage bank (flash sectors 6/7),
 * verifies it, records its CRC + selects it in the shared settings, and resets;
 * the (unchanged, UART-only) bootloader then copies the bank into Exec and runs
 * it. A bad transfer can never boot: fw.commit is the only thing that moves
 * active_bank, and it refuses anything but a fully-received, CRC-matched image
 * with sane vectors. RS-485 YMODEM via the bootloader stays as recovery.
 *
 * Wire flow (host side implemented in tools/dro_update.py --net):
 *   CLI:  fw.begin <bank> <size> <crc32hex8>   -> listens on TCP net.port+1
 *   DATA: host streams exactly <size> raw bytes to net.port+1, then closes
 *   CLI:  fw.status                            -> fw.state=idle|recv|done|error
 *   CLI:  fw.commit                            -> records bank_crc + active_bank
 *   CLI:  reset                                -> bootloader copy-on-activate
 *
 * Flash writes run with interrupts enabled: the motion ISR is RAM-resident
 * (RAM vector table), so steps keep generating through the erase/program —
 * the same guarantee `save` relies on.
 */
#ifndef DRDRO_FWUPDATE_H
#define DRDRO_FWUPDATE_H

#include <stdint.h>
#include "Ramps.h"

typedef enum {
  FW_IDLE = 0,
  FW_RECV,     /* armed/receiving on the data socket */
  FW_DONE,     /* full image received, stream CRC matched */
  FW_ERROR,    /* see FwUpdateReason() */
} fw_state_t;

/* Create the (normally dormant) data-receiver task. Call before the scheduler. */
void FwUpdateStart(rampsSharedData_t *shared);

/* Arm an update: target bank (0|1), exact image size, CRC32 of the image bytes.
 * Opens the data listener on net.port+1. Returns 0 ok; -1 bad args; -2 busy;
 * -3 network down. */
int  FwUpdateBegin(uint8_t bank, uint32_t size, uint32_t crc);

/* Abort: close the data socket, discard session state (bank content undefined
 * but unbootable — vectors incomplete or CRC unrecorded). */
void FwUpdateAbort(void);

fw_state_t  FwUpdateState(void);
uint32_t    FwUpdateReceived(void);
const char *FwUpdateReason(void);   /* valid in FW_ERROR: timeout|crc|flash|size */
uint8_t     FwUpdateBank(void);

/* Commit a FW_DONE image: re-check vectors, CRC the whole 128K region, write
 * bank_crc[bank] + active_bank=bank into settings in ONE save. Returns 0 ok,
 * -1 not ready / invalid image, -2 flash error. On success *regionCrc is the
 * recorded region CRC. */
int FwUpdateCommit(uint32_t *regionCrc);

#endif /* DRDRO_FWUPDATE_H */
