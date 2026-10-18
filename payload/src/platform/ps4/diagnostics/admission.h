// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "platform/ps4/system/firmware.h"
#include <stdio.h>

/* FW11 caps stage only; later slot, fps and source-mode checks still apply.
 * Reconstructed from 0x7c70/0x9d50, restricted to the two tested profiles. */
static inline uint32_t pbs_ps4_admission_caps(uint32_t raw, unsigned profile) {
  uint32_t caps;
  if (profile == 2) {
    uint32_t mask = 0x41067f;
    if (!(raw & 0x10800)) {
      uint32_t owners = raw & ((raw & 0x80) ? 0x140000 : 0x3c0000);
      if (owners & (owners - 1)) mask = 0x54067f;
    }
    caps = raw & mask;
  } else if (profile == 20 || profile == 24) {
    caps = (raw & (0x50a69 | ((raw & 0x80) ? 0 : 0x200000))) ^ 0x200000;
    /* Profile24 here means only the isolated 24/2 request: secondary2
     * avoids the Neo-specific XOR. The original 20/20 path keeps it. */
    if (profile == 20) caps ^= 0x200;
    if (caps & 0x240) return caps;
  } else return UINT32_MAX;
  return caps & ~0x240u;
}
int pbs_ps4_admission_snapshot(FILE *log, const pbs_ps4_firmware *fw,
                                const char *stage, unsigned profile);

/* Bounded read-only snapshot of profile2's failed encoder before Close.
 * Native code c780 publishes the linked-list head; +8 is its next link.
 * No native calls or register/code/flag writes. -2 means detach uncertain. */
int pbs_ps4_encoder_snapshot(FILE *log, const pbs_ps4_firmware *fw,
                              const char *path);
