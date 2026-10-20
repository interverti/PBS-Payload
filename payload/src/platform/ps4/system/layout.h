// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "platform/ps4/system/firmware.h"

typedef int (*pbs_ps4_layout_read)(void *, uint64_t, void *, size_t);

/* Read-only discovery. Addresses come from ELF segments, instruction context
 * and relative control flow; neither routine consults a firmware offset. */
int pbs_ps4_layout_kernel(pbs_ps4_layout_read read, void *opaque,
                          uint64_t base, size_t image_size, pbs_ps4_firmware *out);
intptr_t pbs_ps4_layout_trap(pbs_ps4_layout_read read, void *opaque,
                            intptr_t anchor);
