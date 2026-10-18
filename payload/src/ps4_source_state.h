// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

/* Read-only outputs exposed by SceAvSetting and SceVideoOut. Neither getter
 * measures actual pixel contents, encoder scan-in ownership, nor readiness.
 * The asynchronous settle probe is smoke-only and only changes start timing. */
typedef struct {
  int mode_valid;
  uint64_t base_mode;
  int bus_valid;
  uint8_t bus_enum;
  uint32_t bus_flags;
} pbs_ps4_source_state;

static inline int pbs_ps4_source_observable(const pbs_ps4_source_state *a,
                                            const pbs_ps4_source_state *b) {
  return (a->mode_valid && b->mode_valid) || (a->bus_valid && b->bus_valid);
}

static inline int pbs_ps4_source_changed(const pbs_ps4_source_state *a,
                                         const pbs_ps4_source_state *b) {
  if (a->mode_valid && b->mode_valid && a->base_mode != b->base_mode) return 1;
  if (a->bus_valid && b->bus_valid &&
      (a->bus_enum != b->bus_enum || a->bus_flags != b->bus_flags)) return 1;
  return 0;
}
