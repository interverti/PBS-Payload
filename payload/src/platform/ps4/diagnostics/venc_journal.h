// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "platform/ps4/system/firmware.h"
#include <stdio.h>
#include <stdint.h>
#define PBS_VENC_JOURNAL_DATA_SIZE (8u + 64u * 64u)
/* FW11-only printf diagnostic, never an encoder/error bypass. The original
 * import pointer is restored before Stop/Close. Inert padding is deliberately
 * retained until reboot: a paused thread could still be returning from it. */
int pbs_ps4_venc_journal_apply(FILE *log, const pbs_ps4_firmware *fw);
int pbs_ps4_venc_journal_finish(FILE *log, const char *path);
int pbs_ps4_venc_journal_pending(void);
size_t pbs_ps4_venc_journal_stub(unsigned char *out, uint64_t data_address);
