/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * TCP CLI — the drDRO line protocol served over the W5500, mirroring the
 * RS-485 link byte-for-byte (same framing, same commands, same responses).
 * One client at a time on W5500 socket 1, TCP port net.port (default 5555).
 * The RS-485 turnaround settle and self-echo guard do not apply here.
 */
#ifndef DRDRO_NETCLI_H
#define DRDRO_NETCLI_H

#include "Ramps.h"

/* Create the TCP CLI server task. Call before starting the scheduler, after
 * NetStart (the task waits for the network to come up). */
void NetCliStart(rampsSharedData_t *shared);

#endif /* DRDRO_NETCLI_H */
