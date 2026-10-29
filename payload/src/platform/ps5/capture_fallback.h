// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

/* These firmware/mode pairs were observed on consoles whose automatic 1440p
 * requests were rejected. Prefer 1080p on the first open, avoiding a failed
 * 1440p attempt that may leave the encoder locked. These are observed states,
 * not decoded video-out enums (no inference about HDR, refresh rate or 4K). */
static inline int pbs_ps5_prefer_1080_before_open(
    uint32_t firmware, uint32_t video_mode) {
  return (firmware == 0x12700001u && video_mode == 0x144u) ||
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