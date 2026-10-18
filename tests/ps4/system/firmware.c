// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/system/firmware.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  const pbs_ps4_firmware *p = pbs_ps4_firmware_find(0x11008001u);
  assert(p && pbs_ps4_firmware_find(0x11000000u) == p);
  const uint32_t unsupported[] = {0, 0x09008001u, 0x10008001u, 0x11018001u,
                                 0x11508001u, 0x12008001u, UINT32_MAX};
  for (size_t i = 0; i < sizeof unsupported / sizeof unsupported[0]; ++i)
    assert(!pbs_ps4_firmware_find(unsupported[i]));
  assert(pbs_ps4_firmware_trap(p, 0x800000940) == 0x8000000be);
  assert(!pbs_ps4_firmware_trap(NULL, 0x800000940));
  assert(!pbs_ps4_firmware_trap(p, 0));
  assert(!pbs_ps4_firmware_trap(p, 0x940));
  puts("PS4 profiles: supported family, unsupported versions and trap bounds verified");
  return 0;
}
