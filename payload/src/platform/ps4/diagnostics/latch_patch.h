// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "platform/ps4/system/firmware.h"
#include <stdio.h>
/* Opt-in 1080p30 smoke only. The latch writes wrap one OpenVideoEx window;
 * native Start/Read run after the original bytes return. With
 * PBS_PS4_BUDGET_UNLOCK also confirmed idle at capture_flags=0x100004, the
 * optional budget arm may set the decoder budget bit 0x200 (24 MiB -> 54 MiB
 * core, NPXS21003 +0x240a0) under the same stopped attach/verify/detach
 * discipline; it is released after stop/close so it also covers Start. */
int pbs_ps4_latch_patch_apply(FILE *log, const pbs_ps4_firmware *fw);
int pbs_ps4_latch_patch_restore(FILE *log);
int pbs_ps4_latch_patch_pending(void);
int pbs_ps4_latch_budget_pending(void);
int pbs_ps4_latch_budget_set(FILE *log, unsigned enable);
