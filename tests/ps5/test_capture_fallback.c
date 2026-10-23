// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/capture_fallback.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  assert(pbs_ps5_should_fallback_1080(0x12700001u, 0x81950008u, 0x144u, 2560, 1440));
  assert(pbs_ps5_should_fallback_1080(0x12700001u, 0x81950008u, 0x10u, 2560, 1440));
  assert(pbs_ps5_should_fallback_1080(0x12400009u, 0x81950008u, 0x10u, 2560, 1440));
  assert(!pbs_ps5_should_fallback_1080(0x12400009u, 0x81950008u, 0x144u, 2560, 1440));
  assert(!pbs_ps5_should_fallback_1080(0x12700001u, 0x81950001u, 0x144u, 2560, 1440));
  assert(!pbs_ps5_should_fallback_1080(0x12700001u, 0x81950008u, 0x144u, 1920, 1080));
  assert(!pbs_ps5_should_fallback_1080(0x12700001u, 0x81950008u, 0x144u, 1280, 720));
  puts("PASS: FW12.70 0x144 1080p fallback and no other firmware regression");
  return 0;
}