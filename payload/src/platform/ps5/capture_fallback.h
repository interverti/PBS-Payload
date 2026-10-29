// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

/* Some firmware/output-state combinations reject an automatic 1440p
 * request. Prefer 1080p on the first open: a rejected attempt can leave the
 * encoder locked even after clearing the callback.
 *
 * FW13.42: a user reported OpenVideo=0x81950008, but did not provide the
 * request parameters or video-out mode. It has the same firmware-native
 * request layout as FW13.60 (verified against both memory dumps). Choose the
 * conservative profile regardless of an available video-out mode value,
 * pending hardware confirmation. Explicit 1440p/4K requests are unaffected.
 * The other pairs are directly observed, not decoded HDR/4K enums. */
static inline int pbs_ps5_prefer_1080_before_open(
    uint32_t firmware, uint32_t video_mode) {
  return firmware == 0x13420000u ||
         (firmware == 0x12700001u && video_mode == 0x144u) ||
         (firmware == 0x13600007u && video_mode == 0x14u);
}

/* An actual 0x81950008 rejection of an automatic 1440p request warrants a
 * retry at 1080p, after successful cleanup. The firmware's opaque video mode
 * is not a prerequisite; other errors are deliberately not conflated. */
static inline int pbs_ps5_should_fallback_1080(
    uint32_t firmware, uint32_t open_result, uint32_t video_mode,
    int width, int height) {
  (void)firmware;
  (void)video_mode;
  return width == 2560 && height == 1440 &&
         open_result == 0x81950008u;
}