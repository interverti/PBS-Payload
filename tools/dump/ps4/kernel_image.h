// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "platform/ps4/system/layout.h"
#include <stdio.h>

/* Memory-layout image: ELF headers and file-backed executable LOADs only.
 * Non-executable holes are not acquired; offsets stay relative to image base. */
int pbs_ps4_dump_kernel_code(pbs_ps4_layout_read read, void *opaque,
    uint64_t base, size_t image_size, FILE *image, FILE *report);
