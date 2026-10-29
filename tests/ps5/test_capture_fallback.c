// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/capture_fallback.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  /* Bypass a known-rejected 1440p configuration before touching the encoder. */
  assert(pbs_ps5_prefer_1080_before_open(0x12700001u, 0x144u));
  assert(pbs_ps5_prefer_1080_before_open(0x13600007u, 0x14u));
  assert(!pbs_ps5_prefer_1080_before_open(0x13600007u, 0x144u));
  assert(!pbs_ps5_prefer_1080_before_open(0x12700001u, 0x14u));
  assert(!pbs_ps5_prefer_1080_before_open(0x12400009u, 0x144u));
  assert(!pbs_ps5_prefer_1080_before_open(0x13400001u, 0x14u));

  /* For all firmware, reject a failing automatic 1440p profile once and
   * retry 1080p after cleanup. Do not interpret mode values as HDR enums. */
  assert(pbs_ps5_should_fallback_1080(0x13600007u, 0x81950008u, 0x14u, 2560, 1440));
  assert(pbs_ps5_should_fallback_1080(0x12700001u, 0x81950008u, 0x144u, 2560, 1440));
  assert(pbs_ps5_should_fallback_1080(0x12400009u, 0x81950008u, 0x144u, 2560, 1440));
  assert(pbs_ps5_should_fallback_1080(0x06020004u, 0x81950008u, 0x0u, 2560, 1440));
  assert(!pbs_ps5_should_fallback_1080(0x13600007u, 0x81950001u, 0x14u, 2560, 1440));
  assert(!pbs_ps5_should_fallback_1080(0x13600007u, 0u, 0x14u, 2560, 1440));
  assert(!pbs_ps5_should_fallback_1080(0x13600007u, 0x81950008u, 0x14u, 1920, 1080));
  assert(!pbs_ps5_should_fallback_1080(0x13600007u, 0x81950008u, 0x14u, 3840, 2160));
  puts("PASS: FW13.60/12.70 proactive 1080p, bounded fallback on 1440p rejection");
  return 0;
}
