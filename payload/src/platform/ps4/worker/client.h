// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/* Client side of the dedicated worker thread that runs inside the target.
 *
 * The capture path that stops SceRemotePlay for every Avcap call kept the
 * daemon frozen for the whole session and preceded heap guard corruption
 * (0xAF guard bytes replaced by 0x00 in both heaps). The worker runs the very
 * same calls on a thread we create in the target, with its own stack, so the
 * borrowed thread returns to native work right after the one-time setup.
 *
 * Wire format (pbs_worker_job, see platform/ps4/worker/ipc.h):
 *   payload: function + args, then phase = 1
 *   worker:  calls it, stores result, phase = 2
 *   payload: reads result, next call; phase = 3 asks the thread to leave.
 *
 * The transport that reads/writes the job is abstracted: the PS4 channel
 * cannot be a plain shared page because kernel_copyin/kernel_copyout only
 * address kernel memory (kaddr), never another process' user memory. */
#include <stddef.h>
#include <stdint.h>

#include "platform/ps4/worker/ipc.h"

typedef struct {
  int (*read)(void *opaque, uint64_t address, void *dst, size_t size);
  int (*write)(void *opaque, uint64_t address, const void *src, size_t size);
  void *opaque;
} pbs_worker_io;

typedef struct {
  const pbs_worker_io *io;
  uint64_t job;      /* address of pbs_worker_job in the worker space */
  unsigned timeout_ms;
  int fatal;
} pbs_worker_t;

/* Fill a job image for first use. sleep_fn must accept a microsecond count,
 * unmap_fn the mapping that holds both code and job, so the thread can unmap
 * itself and return into its native pthread caller. */
void pbs_worker_job_init(pbs_worker_job *job, uint64_t sleep_fn,
                         uint64_t unmap_fn, uint64_t map_base,
                         uint64_t map_size);

/* Wait until the worker published ready = 1. 0 on success. */
int pbs_worker_wait_ready(pbs_worker_t *w);

/* Call fn(args[0..5]) on the worker thread; stores the return value. 0 on
 * success, -1 on timeout (w->fatal is set) or transport failure. */
int pbs_worker_call(pbs_worker_t *w, uint64_t fn, const uint64_t args[6],
                    int64_t *result);

/* Ask the worker to leave its loop; the thread unmaps itself and returns. */
int pbs_worker_exit(pbs_worker_t *w);
/* Socket transport: the worker connects to a loopback listener owned by the
 * payload, so no ptrace read is needed while the target runs. */
#include <sys/socket.h>

typedef struct {
  int listen_fd;
  int fd;
  uint32_t port;
  uint32_t cookie;
  int fatal;
} pbs_worker_link;

/* Listen on 127.0.0.1:port_hint (0 picks an ephemeral port). */
int pbs_worker_link_open(pbs_worker_link *link, unsigned port_hint);
/* Wait for the worker connection and verify the handshake cookie. */
int pbs_worker_link_accept(pbs_worker_link *link, unsigned timeout_ms);
/* Run fn(args[0..5]) on the worker thread. */
int pbs_worker_link_call(pbs_worker_link *link, uint64_t fn,
                         const uint64_t args[6], int64_t *result,
                         unsigned timeout_ms);
/* Copy size bytes of target memory into dst through the worker. */
int pbs_worker_link_copy(pbs_worker_link *link, uint64_t address, void *dst,
                         uint32_t size, unsigned timeout_ms);
int pbs_worker_link_write(pbs_worker_link *link, uint64_t address,
                          const void *src, uint32_t size, unsigned timeout_ms);
int pbs_worker_link_scratch(pbs_worker_link *link, uint64_t *address);
int pbs_worker_link_exit(pbs_worker_link *link);
void pbs_worker_link_close(pbs_worker_link *link);
