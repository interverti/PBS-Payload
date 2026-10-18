// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdio.h>
#include "common/process/inject.h"
/* request_active tracks whether a successful SetStaticInfo(21) was observed,
 * even if a subsequent ptrace/kernel-gate restoration fails. */
int pbs_ps4_control(FILE *log, int enable, int *request_active);
/* Retained owner: ptrace during bootstrap, then a detached socket worker.
 * Keep this context until SetStaticInfo(0)/Term and worker shutdown finish. */
pbs_inject_t *pbs_ps4_control_ctx(void);
int pbs_ps4_control_ctx_release(void);
/* Page-aligned scratch validated by the most recent control attach. The
 * capture backend tries it first (if still safely below the new RSP) before
 * searching for a fresh page. */
intptr_t pbs_ps4_control_last_scratch(void);
/* Cleanup must distinguish a pending remote Init/Term from temporary patches
 * made by this process. Restoring gates never touches RemotePlay state. */
int pbs_ps4_control_remote_init_pending(void);
int pbs_ps4_control_remote_term_uncertain(void);
/* A timed-out remote call can have executed despite losing its return value.
 * These flags must never be treated as a definite permission to clear an
 * unknown native owner's global state or decrement a remote reference. */
int pbs_ps4_control_ownership_uncertain(void);
int pbs_ps4_control_gates_pending(void);
int pbs_ps4_control_restore_gates(FILE *log);
/* Patch the FW11 debug gates without attaching (required before PT_ATTACH). */
int pbs_ps4_gates_arm(FILE *log);
