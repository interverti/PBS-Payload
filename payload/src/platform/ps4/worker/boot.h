// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdio.h>
#include "common/process/inject.h"

/* Start the dedicated worker thread inside the target while the caller still
 * holds the attachment, then detach and redirect all injected calls and remote
 * memory accesses to it. Returns 0 when the worker is connected and the hooks
 * are installed. Failure after thread creation leaves the target detached.
 * Existing cave code is retained: socket EOF does not prove thread return.
 * The pthread is detached so its native resources are reclaimed on return. */
int pbs_ps4_worker_boot(pbs_inject_t *ctx, FILE *log);

/* Request exit, wait for socket EOF, and remove hooks. Idempotent.
 * Does not overwrite live target code. */
int pbs_ps4_worker_shutdown(void);
