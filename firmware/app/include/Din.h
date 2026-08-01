/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Digital (opto-isolated) inputs ISO_IN1..6 — TLP2309 open-collector outputs
 * that sink when the input is driven, so the MCU pins use internal pull-ups
 * and read LOW = active. A low-priority task samples every millisecond and
 * debounces with a per-pin integrator sized by shared->din.debounceMs;
 * results land in shared->din (state bitmask + rising-edge counters), which
 * the registry exposes as din.state / din.cnt.
 */
#ifndef DRDRO_DIN_H
#define DRDRO_DIN_H

#include "Ramps.h"

/* Configure the six input pins (pull-ups) and start the poll task. */
void DinStart(rampsSharedData_t *shared);

#endif /* DRDRO_DIN_H */
