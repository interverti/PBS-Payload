// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/* Target-side RPC loop for the socket transport.
 *
 * This code is copied byte for byte into an executable mapping of the target
 * and started as a pthread entry, so it must be position independent and must
 * not reference any external symbol: every libc call goes through a pointer in
 * pbs_worker_remote_ctx, and no string literal is used.
 *
 * The mapping is a code cave in an r-x page: READ + EXECUTE, not writable.
 * The payload fills the context with PT_IO while the target is stopped; after
 * the thread starts, the context is read-only and all mutable state lives on
 * the worker's own stack. A store through ctx in the target faults with
 * "user write data, protection violation" (hardware, 18/10/2026).
 *
 * Wire format, little endian, fixed size request header:
 *   0x00 u32 cmd      1 = call, 2 = exit, 4 = copy memory to the payload
 *   0x04 u32 pad
 *   0x08 u64 fn       function for cmd 1, source address for cmd 4
 *   0x10 u64 a0..a5   six arguments (cmd 1 only)
 *   0x40 u32 len      bytes to copy (cmd 4)
 *
 * Reply: 0x00 i64 result, 0x08 u32 len, then len raw bytes. */
#define PBS_WORKER_REQUEST_SIZE 0x48u
#define PBS_WORKER_REPLY_SIZE 0x10u
#define PBS_WORKER_CMD_CALL 1u
#define PBS_WORKER_CMD_EXIT 2u
#define PBS_WORKER_CMD_COPY 4u
#define PBS_WORKER_CMD_WRITE 5u
#define PBS_WORKER_CMD_SCRATCH 6u
#define PBS_WORKER_WRITE_MAX 0x1000u
/* Largest copy served in one reply; the payload keeps it below its buffer. */
#define PBS_WORKER_COPY_MAX 0x200000u

#ifndef __ASSEMBLER__
#include <stdint.h>

/* Read-only after the payload writes it with PT_IO: no status fields. */
typedef struct {
  uint64_t fn_socket;   /* 0x00 */
  uint64_t fn_connect;  /* 0x08 */
  uint64_t fn_recv;     /* 0x10 */
  uint64_t fn_send;     /* 0x18 */
  uint64_t fn_close;    /* 0x20 */
  uint64_t fn_sleep;    /* 0x28 */
  uint64_t fn_setsockopt;
  uint32_t cookie;      /* 0x38 */
  unsigned char addr[16]; /* 0x3c sockaddr_in, network order, ready to use */
} pbs_worker_remote_ctx;

_Static_assert(sizeof(pbs_worker_remote_ctx) == 0x50, "remote ctx ABI");

/* Entry point copied into the target. arg is a pbs_worker_remote_ctx. */
void pbs_worker_remote_main(void *arg);

/* Bounds of the machine code to copy. The linker defines them for the
 * undotted section name pbs_worker_remote. */
extern const unsigned char __start_pbs_worker_remote[],
    __stop_pbs_worker_remote[];
#endif
