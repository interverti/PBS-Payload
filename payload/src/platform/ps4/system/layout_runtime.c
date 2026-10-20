// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/system/layout.h"
#include <ps4/kernel.h>

static int read_kernel(void *opaque, uint64_t at, void *out, size_t size) {
  (void)opaque;
  return kernel_copyout((intptr_t)at,out,size);
}

const pbs_ps4_firmware *pbs_ps4_firmware_find(uint32_t version) {
  static pbs_ps4_firmware layout;
  static uint64_t resolved_base;
  if (!KERNEL_ADDRESS_IMAGE_BASE || !KERNEL_IMAGE_SIZE) return NULL;
  if (resolved_base) return resolved_base==(uint64_t)KERNEL_ADDRESS_IMAGE_BASE &&
                            layout.version==version ? &layout : NULL;
  if (pbs_ps4_layout_kernel(read_kernel,NULL,(uint64_t)KERNEL_ADDRESS_IMAGE_BASE,
                            KERNEL_IMAGE_SIZE,&layout)) return NULL;
  layout.version=version; layout.version_mask=UINT32_MAX;
  resolved_base=(uint64_t)KERNEL_ADDRESS_IMAGE_BASE;
  return &layout;
}
