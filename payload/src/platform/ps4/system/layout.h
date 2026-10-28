// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "platform/ps4/system/firmware.h"

typedef int (*pbs_ps4_layout_read)(void *, uint64_t, void *, size_t);

/* KERNEL_ADDRESS_IMAGE_BASE is an intptr_t exported by libkernel and the PS4
 * kernel lives in the upper half of the address space, so the value is negative
 * on every firmware. Only zero means "the SDK did not resolve it"; never test
 * it with "> 0", which silently disables discovery on real hardware. */
static inline int pbs_ps4_layout_available(intptr_t base, size_t image_size) {
  return base != 0 && image_size != 0;
}

/* Read-only discovery. Addresses come from ELF segments, instruction context
 * and relative control flow; neither routine consults a firmware offset. */
int pbs_ps4_layout_kernel(pbs_ps4_layout_read read, void *opaque,
                          uint64_t base, size_t image_size, pbs_ps4_firmware *out);
intptr_t pbs_ps4_layout_trap(pbs_ps4_layout_read read, void *opaque,
                            intptr_t anchor);
