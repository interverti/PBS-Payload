// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifndef PBS_PS5_CONTROLLER
#define PBS_PS5_CONTROLLER 1
#endif

#if PBS_PS5_CONTROLLER != 0 && PBS_PS5_CONTROLLER != 1
#error "PBS_PS5_CONTROLLER must be 0 or 1"
#endif

#if PBS_PS5_CONTROLLER
void pbs_ps5_controller_configure(void);
#else
static inline void pbs_ps5_controller_configure(void) {
  /* The common HTTP routes report unsupported without a configured backend. */
}
#endif
