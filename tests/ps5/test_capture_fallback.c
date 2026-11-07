// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/capture_fallback.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  /* Bypass a known-rejected 1440p configuration before touching the encoder. */
  assert(pbs_ps5_prefer_1080_before_open(0x12700001u, 0x144u));
  assert(pbs_ps5_prefer_1080_before_open(0x13600007u, 0x14u));
  assert(pbs_ps5_prefer_1080_before_open(0x12400009u, 0x140u));
  assert(!pbs_ps5_prefer_1080_before_open(0x12400009u, 0x138u));
  assert(!pbs_ps5_prefer_1080_before_open(0x09400008u, 0x140u));
  /* 13.42 lacks a video-mode log: choose a conservative automatic startup
   * for either possible value, including an unavailable preflight query. */
  assert(pbs_ps5_prefer_1080_before_open(0x13420000u, 0u));
  assert(pbs_ps5_prefer_1080_before_open(0x13420000u, 0x14u));
  assert(pbs_ps5_prefer_1080_before_open(0x13420000u, 0x144u));
  assert(!pbs_ps5_prefer_1080_before_open(0x13600007u, 0x144u));
  assert(!pbs_ps5_prefer_1080_before_open(0x12700001u, 0x14u));
  assert(!pbs_ps5_prefer_1080_before_open(0x12400009u, 0x144u));
  assert(!pbs_ps5_prefer_1080_before_open(0x13400001u, 0x14u));
  assert(pbs_ps5_prefer_1080_before_open(0x10010000u, 0u));
  assert(pbs_ps5_prefer_1080_before_open(0x10010000u, 0x14u));

  assert(pbs_ps5_can_reopen_capture(1, 0, 1, 1));
  assert(!pbs_ps5_can_reopen_capture(1, 0, 0, 1));
  assert(!pbs_ps5_can_reopen_capture(1, 1, 1, 1));
  assert(!pbs_ps5_can_reopen_capture(0, 0, 1, 1));
  assert(!pbs_ps5_can_reopen_capture(1, 0, 1, 0));
  puts("PASS: proactive 1080p startup and rejected OpenVideo quarantine");
  return 0;
}
