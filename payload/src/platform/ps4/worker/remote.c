// SPDX-License-Identifier: GPL-3.0-or-later
/* Target-side RPC loop. Copied into the target's own executable mapping, so:
 *  - every libc call is an indirect call through pbs_worker_remote_ctx;
 *  - no string literal, no global variable, no extern symbol is referenced;
 *  - the whole function lives alone in .pbs_worker_remote, so the payload can
 *    copy [start, end) with a single memcpy.
 *
 * The mapping is a code cave inside the target's r-x segment: readable and
 * executable, NOT writable by the thread. On 18/10/2026 the payload wrote the
 * context into that same page, the worker started correctly, and its first
 * store (ready = 10) raised SIGSEGV "page fault (user write data, protection
 * violation)" in SceRemotePlay. Therefore:
 *  - pbs_worker_remote_ctx is read-only at run time; the payload fills it with
 *    PT_IO before the thread starts;
 *  - every mutable value (fd, request, reply, counters) lives on the worker
 *    thread's own stack, which is writable.
 * Do not reintroduce a store through ctx or any pointer into the cave page. */
#include "platform/ps4/worker/remote.h"
#include <sys/socket.h>

#define PBS_WORKER_ASM_SECTION \
  __attribute__((section("pbs_worker_remote"), noinline, used))

typedef int (*worker_socket_fn)(int, int, int);
typedef int (*worker_connect_fn)(int, const void *, unsigned);
typedef long (*worker_recv_fn)(int, void *, unsigned long, int);
typedef long (*worker_send_fn)(int, const void *, unsigned long, int);
typedef int (*worker_close_fn)(int);
typedef int (*worker_sleep_fn)(unsigned);

static int worker_read_full(worker_recv_fn recv_fn, int fd, void *buffer,
                            unsigned size) {
  unsigned char *out = buffer;
  unsigned done = 0;
  while (done < size) {
    long n = recv_fn(fd, out + done, size - done, 0);
    if (n <= 0) return -1;
    done += (unsigned)n;
  }
  return 0;
}

static int worker_send_full(worker_send_fn send_fn, int fd, const void *buffer,
                            unsigned size) {
  const unsigned char *in = buffer;
  unsigned done = 0;
  while (done < size) {
    long n = send_fn(fd, in + done, size - done, MSG_NOSIGNAL);
    if (n <= 0) return -1;
    done += (unsigned)n;
  }
  return 0;
}

PBS_WORKER_ASM_SECTION void pbs_worker_remote_main(void *arg) {
  const pbs_worker_remote_ctx *ctx = arg;
  const worker_socket_fn socket_fn = (worker_socket_fn)(uintptr_t)ctx->fn_socket;
  const worker_connect_fn connect_fn =
      (worker_connect_fn)(uintptr_t)ctx->fn_connect;
  const worker_recv_fn recv_fn = (worker_recv_fn)(uintptr_t)ctx->fn_recv;
  const worker_send_fn send_fn = (worker_send_fn)(uintptr_t)ctx->fn_send;
  const worker_close_fn close_fn = (worker_close_fn)(uintptr_t)ctx->fn_close;
  const worker_sleep_fn sleep_fn = (worker_sleep_fn)(uintptr_t)ctx->fn_sleep;
  const uint32_t cookie = ctx->cookie;
  const unsigned char *addr = ctx->addr;
  /* Persistent until EXIT; never borrow a running native thread stack. */
  volatile unsigned char scratch[PBS_WORKER_WRITE_MAX];
  unsigned char request[PBS_WORKER_REQUEST_SIZE];
  unsigned char reply[PBS_WORKER_REPLY_SIZE];

  int fd = -1;
  for (unsigned attempt = 0; attempt < 600; ++attempt) {
    fd = socket_fn(2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
    if (fd < 0) {
      sleep_fn(1000);
      continue;
    }
    int one = 1;
    int (*setopt)(int,int,int,const void *,unsigned) =
        (int (*)(int,int,int,const void *,unsigned))(uintptr_t)ctx->fn_setsockopt;
    if (!setopt || setopt(fd, 6 /* IPPROTO_TCP */, 1 /* TCP_NODELAY */,
                         &one, sizeof one)) { close_fn(fd); return; }
    if (connect_fn(fd, addr, 16) == 0) break;
    close_fn(fd);
    fd = -1;
    sleep_fn(1000);
  }
  if (fd < 0) return;
  if (worker_send_full(send_fn, fd, &cookie, sizeof cookie)) {
    close_fn(fd);
    return;
  }

  for (;;) {
    if (worker_read_full(recv_fn, fd, request, PBS_WORKER_REQUEST_SIZE)) break;
    uint32_t wire_cmd;
    __builtin_memcpy(&wire_cmd, request, sizeof wire_cmd);
    /* Prevent a compiler-generated jump table outside this relocated blob. */
    volatile uint32_t cmd = wire_cmd;
    uint64_t fn;
    __builtin_memcpy(&fn, request + 0x08, sizeof fn);
    uint64_t result = 0;
    uint32_t len = 0;
    if (cmd == PBS_WORKER_CMD_SCRATCH) {
      result = (uint64_t)(uintptr_t)scratch;
    } else if (cmd == PBS_WORKER_CMD_CALL) {
      uint64_t a[6];
      __builtin_memcpy(a, request + 0x10, sizeof a);
      long (*target)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) =
          (long (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                    uint64_t))(uintptr_t)fn;
      result = (uint64_t)target(a[0], a[1], a[2], a[3], a[4], a[5]);
    } else if (cmd == PBS_WORKER_CMD_COPY) {
      __builtin_memcpy(&len, request + 0x40, sizeof len);
      if (len > PBS_WORKER_COPY_MAX) len = PBS_WORKER_COPY_MAX;
    } else if (cmd == PBS_WORKER_CMD_WRITE) {
      uint32_t want = 0;
      __builtin_memcpy(&want, request + 0x40, sizeof want);
      unsigned char tmp[PBS_WORKER_WRITE_MAX];
      if (want == 0 || want > PBS_WORKER_WRITE_MAX ||
          worker_read_full(recv_fn, fd, tmp, want)) break;
      /* Variable-size memcpy becomes a call through the payload GOT with
       * Orbis clang. That address is invalid after copying this section.
       * Volatile stores keep this small bounded copy entirely in the blob. */
      volatile unsigned char *dst = (volatile unsigned char *)(uintptr_t)fn;
      for (uint32_t i = 0; i < want; ++i) dst[i] = tmp[i];
      result = want;
    }
    __builtin_memcpy(reply, &result, sizeof result);
    __builtin_memcpy(reply + 0x08, &len, sizeof len);
    if (worker_send_full(send_fn, fd, reply, PBS_WORKER_REPLY_SIZE)) break;
    if (cmd == PBS_WORKER_CMD_COPY && len)
      if (worker_send_full(send_fn, fd, (const void *)(uintptr_t)fn, len)) break;
    if (cmd == PBS_WORKER_CMD_EXIT) break;
  }
  close_fn(fd);
}
