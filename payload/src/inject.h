// SPDX-License-Identifier: GPL-3.0-or-later
/* PBS - remote process calls.
 * Uses a ptrace technique analogous to prospero-menu (libNineS / John Tornblom,
 * GPL-3.0-or-later) and MemDBG: temporarily elevate caller credentials, stop
 * the target, invoke a function with SysV arguments, then restore registers
 * and detach. Scratch memory is reserved below the stopped thread stack. */
#pragma once

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

typedef struct {
  pid_t pid;
  intptr_t scratch;    /* Writable target memory below the stack. */
  size_t scratch_len;
  int attached;
  int asserted;        /* The last call triggered an assertion. */
} pbs_inject_t;

/* Return value for a remote call that did not complete. */
#define PBS_CALL_TIMEOUT LONG_MIN

/* Find a process by name. */
pid_t pbs_find_pid(const char *name);

/* Resolve a module export by symbol NID in the target process. */
intptr_t pbs_resolve_in(pid_t pid, const char *module, const char *symbol);

/* Attach and prepare scratch memory; return zero on success. */
int pbs_inject_begin(const char *proc_name, pbs_inject_t *ctx);

/* Call a function with up to six arguments; return LONG_MIN on timeout. */
long pbs_inject_call(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]);
/* Run to a return breakpoint instead of single-stepping. */
long pbs_inject_call_fast(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]);
/* Fast call with XMM0 set to the first float argument. */
long pbs_inject_call_fast_fp(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6], float xmm0);

/* Copy from or to target scratch memory. */
int pbs_inject_read(pbs_inject_t *ctx, void *out, size_t len);
int pbs_inject_write(pbs_inject_t *ctx, const void *in, size_t len);

/* Read from an arbitrary target address. */
int pbs_inject_read_at(pbs_inject_t *ctx, intptr_t addr, void *out, size_t len);

/* Write to an arbitrary target address. */
int pbs_inject_write_at(pbs_inject_t *ctx, intptr_t addr, const void *in, size_t len);

/* Detach; safe to call after partial initialization. */
void pbs_inject_end(pbs_inject_t *ctx);
