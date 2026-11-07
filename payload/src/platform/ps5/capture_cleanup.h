// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

typedef enum { PBS_PS5_RELEASE_STOP, PBS_PS5_RELEASE_CALLBACK,
               PBS_PS5_RELEASE_INITIALIZE } pbs_ps5_release_step;
typedef struct {
  int initialized, callback, running, uncertain;
} pbs_ps5_capture_ownership;
typedef int (*pbs_ps5_release_call)(void *, pbs_ps5_release_step);

/* ResourceArbitrator returns this when the feature already has a callback.
 * The FW4.00 implementation emits it from the exact-key collision path in
 * SetCallback's internal tree. Treat the callback as borrowed, never owned. */
static inline int pbs_ps5_callback_already_registered(long result) {
  return (uint32_t)result == 0x819b0007u;
}

/* Release only this attempt's acquisitions, after its handle has closed.
 * STOP is the resource-arbitrator state used by native RemotePlay, not an
 * Avcap Stop call. A failure must prevent another Open/Initialize attempt. */
static inline int pbs_ps5_release_capture(pbs_ps5_capture_ownership *owned,
    int handle_closed, pbs_ps5_release_call invoke, void *opaque) {
  if (!handle_closed || owned->uncertain) return -1;
  int *flags[] = {&owned->running, &owned->callback, &owned->initialized};
  for (unsigned step=0;step<3;step++) {
    if (!*flags[step]) continue;
    if (!invoke || invoke(opaque,(pbs_ps5_release_step)step)) {
      owned->uncertain=1;
      return -1;
    }
    *flags[step]=0;
  }
  return 0;
}
