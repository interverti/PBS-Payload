// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

/* SceAvCapture's actual resolution tables, not HDMI output-mode values.
 * 3.20/4.00/4.51/5.00/5.10/5.50: 480,640,960,1280,1920,3840 (indices 0..5).
 * 6.02 and later add 2560 before 3840 (indices 0..6).
 * Only the measured early exact profiles use the legacy mapping. */
static inline int pbs_ps5_legacy_resolution_modes(uint32_t firmware) {
  return firmware==0x03200004u || firmware==0x04000042u ||
         firmware==0x04510001u || firmware==0x05000040u ||
         firmware==0x05100023u || firmware==0x05500008u;
}
static inline int pbs_ps5_resolution_mode(uint32_t firmware,int height,uint32_t *mode) {
  uint32_t value;
  switch (height) {
    case 720: value=3; break;
    case 1080: value=4; break;
    case 1440:
      if (pbs_ps5_legacy_resolution_modes(firmware)) return -1;
      value=5; break;
    case 2160: value=pbs_ps5_legacy_resolution_modes(firmware)?5:6; break;
    default: return -1;
  }
  if (mode) *mode=value;
  return 0;
}
