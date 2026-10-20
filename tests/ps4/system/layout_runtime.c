// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/system/layout.h"
#include <ps4/kernel.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int scans, reject=1;
int kernel_copyout(intptr_t at, void *out, size_t size) {
  (void)at; (void)out; (void)size; return -1;
}
int pbs_ps4_layout_kernel(pbs_ps4_layout_read read, void *opaque,
                          uint64_t base, size_t size, pbs_ps4_firmware *out) {
  assert(read && !opaque && base==KERNEL_ADDRESS_IMAGE_BASE && size==KERNEL_IMAGE_SIZE);
  ++scans;
  if (reject) return -1;
  memset(out,0,sizeof *out);
  out->name="mock runtime evidence";
  out->gates[0].offset=0x1234; out->gates[1].offset=0x5678;
  return 0;
}
int main(void) {
  assert(!pbs_ps4_firmware_find(0x12008001u) && scans==1);
  reject=0;
  const pbs_ps4_firmware *p=pbs_ps4_firmware_find(0x12008001u);
  assert(p && scans==2 && p->version==0x12008001u && p->gates[0].offset==0x1234);
  assert(pbs_ps4_firmware_find(0x12008001u)==p && scans==2);
  assert(!pbs_ps4_firmware_find(0x11008001u) && scans==2);
  assert(!pbs_ps4_firmware_trap(p,0x10000940)); /* No implicit legacy fallback. */
  puts("Runtime layout cache: code evidence required, no firmware whitelist, failed discovery unpublished and legacy trap fallback disabled");
  return 0;
}
