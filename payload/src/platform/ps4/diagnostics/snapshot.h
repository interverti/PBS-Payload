// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct {
  intptr_t address;
  void *destination;
  size_t size;
} pbs_ps4_snapshot_region;
/* Brief stop/read/detach only. No scratch, register writes or remote calls.
 * Caller must already own the debug gates; no credentials are changed here.
 * Returns -2 if detach failed (SIGCONT attempted), -1 for other failures. */
int pbs_ps4_read_snapshot(pid_t pid, uint64_t epoch,
                          const pbs_ps4_snapshot_region *regions, size_t count);
