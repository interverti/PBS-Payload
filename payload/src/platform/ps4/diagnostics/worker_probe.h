// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdio.h>
#include "common/process/inject.h"

/* Attach directly, arm the FW11 padding trampoline, boot the shared worker,
 * verify getpid over ptrace and then over loopback, exit. Calling this does
 * not invoke any Avcap API or the control/capture paths. The caller supplies a
 * zeroed context and a log opened by the payload. */
int pbs_ps4_worker_probe(pbs_inject_t *ctx, FILE *log);
