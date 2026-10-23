// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

/* A rejected 1440p OpenVideo can be retried with feature 12 at 1080p.
 * Firmware 12.70 has reported videoOutMode 0x144 at runtime; older supported
 * configurations report 0x10. This is a fallback policy, not a diagnosis
 * of what the opaque 0x81950008 return code means. */
static inline int pbs_ps5_should_fallback_1080(
    uint32_t firmware, uint32_t open_result, uint32_t video_mode,
    int width, int height) {
  return width == 2560 && height == 1440 &&
         open_result == 0x81950008u &&
         (video_mode == 0x10u ||
          (firmware == 0x12700001u && video_mode == 0x144u));
}