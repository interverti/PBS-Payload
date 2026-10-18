// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/* Shared ABI between the payload and a normal pthread inside RemotePlay.
 * Only x86-64 is supported. No kernel object layouts or remote stack storage. */
#define PBS_WORKER_JOB_OFF 0xd00
#define PBS_WORKER_CODE_OFF 0x4000
#define PBS_WORKER_OUTPUT_OFF 0x8000
#define PBS_WORKER_OUTPUT_SIZE 0x200000
#define PBS_WORKER_MAP_SIZE (PBS_WORKER_OUTPUT_OFF + PBS_WORKER_OUTPUT_SIZE)
#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
typedef struct {
  _Atomic uint32_t phase; /* 0 idle, 1 call, 2 complete, 3 exit, 4 exiting */
  _Atomic uint32_t ready;
  int64_t result;
  uint64_t function;
  uint64_t args[6];
  uint64_t sleep_fn;
  uint64_t unmap_fn;
  uint64_t map_base;
  uint64_t map_size;
} pbs_worker_job;
_Static_assert(offsetof(pbs_worker_job, result) == 8, "worker result ABI");
_Static_assert(offsetof(pbs_worker_job, args) == 24, "worker argument ABI");
_Static_assert(offsetof(pbs_worker_job, sleep_fn) == 72, "worker sleep ABI");
_Static_assert(offsetof(pbs_worker_job, map_size) == 96, "worker mapping ABI");
_Static_assert(sizeof(pbs_worker_job) == 104, "worker shared ABI");
extern const unsigned char pbs_worker_code_start[], pbs_worker_code_end[];
#endif
