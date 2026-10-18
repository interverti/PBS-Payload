// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps4_worker_boot.h"

#include <string.h>

#include "ps4_worker.h"
#include "ps4_worker_remote.h"
#include "ps4_worker_addr.h"
#include "ps4_call_trampoline.h"

#define BOOT_THREAD_OFF 0x700u
_Static_assert(BOOT_THREAD_OFF + sizeof(uint64_t) <= PBS_PS4_CALL_STATE_OFF,
               "pthread output must not overwrite saved SIMD state");

static pbs_worker_link boot_link = {.listen_fd = -1, .fd = -1};
static int boot_active;

static long hook_call(intptr_t fn, const uint64_t args[6]) {
  int64_t value = 0;
  if (pbs_worker_link_call(&boot_link, (uint64_t)fn, args, &value, 4000))
    return PBS_CALL_TIMEOUT;
  return (long)value;
}

static int hook_read(intptr_t addr, void *out, size_t len) {
  if (!len || len > PBS_WORKER_COPY_MAX) return -1;
  return pbs_worker_link_copy(&boot_link, (uint64_t)addr, out, (uint32_t)len,
                              4000);
}

static int hook_write(intptr_t addr, const void *in, size_t len) {
  if (!len || len > PBS_WORKER_WRITE_MAX) return -1;
  return pbs_worker_link_write(&boot_link, (uint64_t)addr, in, (uint32_t)len,
                               4000);
}

static intptr_t resolve_first(pbs_inject_t *ctx, FILE *log,
                              const char *const *symbols, const char *label) {
  static const char *const libs[] = {"libkernel_sys.sprx",
                                     "libSceLibcInternal.sprx", "libkernel.sprx",
                                     NULL};
  for (size_t m = 0; libs[m]; ++m)
    for (size_t s = 0; symbols[s]; ++s) {
      intptr_t fn = pbs_resolve_in(ctx->pid, libs[m], symbols[s]);
      if (fn) {
        fprintf(log, "worker boot: %s -> %s!%s fn=%p\n", label, libs[m],
                symbols[s], (void *)fn);
        return fn;
      }
    }
  fprintf(log, "worker boot: %s unresolved\n", label);
  return 0;
}


/* The validated PS4 bootstrap uses existing executable padding. The worker
 * therefore lives in a
 * code cave: a run of padding bytes inside an executable mapping the target
 * already owns. We locate it by scanning around a known export of each module
 * we already resolved. */
static intptr_t find_code_cave(pbs_inject_t *ctx, FILE *log, size_t need) {
  static const struct {
    const char *module;
    const char *symbol;
  } probes[] = {
      {"libSceAvcap.sprx", "sceVshAvcapInit"},
      {"libkernel_sys.sprx", "getpid"},
      {"libSceLibcInternal.sprx", "malloc"},
  };
  unsigned char block[0x1000];
  for (size_t p = 0; p < sizeof probes / sizeof probes[0]; ++p) {
    intptr_t anchor = pbs_resolve_in(ctx->pid, probes[p].module, probes[p].symbol);
    if (!anchor) continue;
    for (intptr_t start = anchor - 0x10000; start < anchor + 0x10000;
         start += (intptr_t)sizeof block) {
      if (start <= 0) continue;
      if (pbs_inject_read_at(ctx, start, block, sizeof block)) continue;
      size_t run = 0;
      for (size_t i = 0; i < sizeof block; ++i) {
        const unsigned char b = block[i];
        if (b == 0x00u || b == 0xccu) {
          ++run;
          if (run >= need) {
            const intptr_t cave = start + (intptr_t)(i + 1 - run);
            fprintf(log, "worker boot: code cave %p size>=%zu in %s near %p\n",
                    (void *)cave, need, probes[p].module, (void *)anchor);
            return cave;
          }
        } else {
          run = 0;
        }
      }
    }
    fprintf(log, "worker boot: no %zu-byte cave in %s\n", need, probes[p].module);
  }
  fprintf(log, "worker boot: no code cave found\n");
  return 0;
}

int pbs_ps4_worker_boot(pbs_inject_t *ctx, FILE *log) {
  if (!ctx || !log || !ctx->attached || !ctx->firmware || boot_active) return -1;
  const size_t blob =
      (size_t)(__stop_pbs_worker_remote - __start_pbs_worker_remote);
  if (blob < 32 || blob > 0x1000u) {
    fprintf(log, "worker boot: bad blob size %zu\n", blob);
    return -1;
  }
  if (pbs_worker_link_open(&boot_link, 0)) {
    fprintf(log, "worker boot: listener failed\n");
    return -1;
  }
  fprintf(log, "worker boot: listening port=%u cookie=%08x blob=%zu\n",
          boot_link.port, boot_link.cookie, blob);

  static const char *const thread_names[] = {"pthread_create",
                                             "scePthreadCreate", NULL};
  static const char *const detach_names[] = {"pthread_detach", "scePthreadDetach", NULL};
  static const char *const socket_names[] = {"socket", NULL};
  static const char *const connect_names[] = {"connect", NULL};
  static const char *const recv_names[] = {"recv", NULL};
  static const char *const send_names[] = {"send", NULL};
  static const char *const close_names[] = {"close", NULL};
  static const char *const setopt_names[] = {"setsockopt", NULL};
  static const char *const sleep_names[] = {"usleep", NULL};

  intptr_t fn_create = resolve_first(ctx, log, thread_names, "pthread_create");
  intptr_t fn_detach = resolve_first(ctx, log, detach_names, "pthread_detach");
  intptr_t fn_socket = resolve_first(ctx, log, socket_names, "socket");
  intptr_t fn_connect = resolve_first(ctx, log, connect_names, "connect");
  intptr_t fn_recv = resolve_first(ctx, log, recv_names, "recv");
  intptr_t fn_send = resolve_first(ctx, log, send_names, "send");
  intptr_t fn_close = resolve_first(ctx, log, close_names, "close");
  intptr_t fn_setopt = resolve_first(ctx, log, setopt_names, "setsockopt");
  intptr_t fn_sleep = resolve_first(ctx, log, sleep_names, "usleep");
  if (!fn_detach || !fn_create || !fn_socket || !fn_connect || !fn_recv ||
      !fn_send || !fn_close || !fn_sleep || !fn_setopt) {
    fprintf(log, "worker boot: missing target exports\n");
    pbs_worker_link_close(&boot_link);
    return -1;
  }

  /* Use only the already-mapped code cave path validated on hardware.
   * No speculative mmap fallback or anonymous mapping to leak on failure. */
  const size_t context_off = (blob + 0xfu) & ~(size_t)0xfu;
  intptr_t map = find_code_cave(ctx, log, blob + 0x100u);
  if (!map) {
    pbs_worker_link_close(&boot_link);
    return -1;
  }
  const intptr_t arg = map + (intptr_t)context_off;
  long rc = 0;
  fprintf(log, "worker boot: worker entry=%p ctx=%p\n", (void *)map,
          (void *)arg);

  pbs_worker_remote_ctx remote;
  memset(&remote, 0, sizeof remote);
  remote.fn_socket = (uint64_t)fn_socket;
  remote.fn_connect = (uint64_t)fn_connect;
  remote.fn_recv = (uint64_t)fn_recv;
  remote.fn_send = (uint64_t)fn_send;
  remote.fn_close = (uint64_t)fn_close;
  remote.fn_sleep = (uint64_t)fn_sleep;
  remote.fn_setsockopt = (uint64_t)fn_setopt;
  remote.cookie = boot_link.cookie;
  pbs_ps4_worker_addr(remote.addr, boot_link.port);
  if (pbs_inject_write_at(ctx, map, __start_pbs_worker_remote, blob) ||
      pbs_inject_write_at(ctx, arg, &remote, sizeof remote)) {
    fprintf(log, "worker boot: copy failed\n");
    pbs_worker_link_close(&boot_link);
    return -1;
  }
  rc = pbs_inject_call_fast(ctx, fn_create,
                            (uint64_t[6]){(uint64_t)(ctx->scratch + BOOT_THREAD_OFF), 0,
                                          (uint64_t)map, (uint64_t)arg, 0, 0});
  if (rc == PBS_CALL_TIMEOUT || (int32_t)rc != 0) {
    fprintf(log, "worker boot: pthread_create rc=%016llx\n",
            (unsigned long long)rc);
    pbs_worker_link_close(&boot_link);
    return -1;
  }
  uint64_t thread = 0;
  int thread_read_failed = pbs_inject_read_at(ctx, ctx->scratch + BOOT_THREAD_OFF,
                                             &thread, sizeof thread);
  fprintf(log, "worker boot: thread created, detaching\n");
  pbs_inject_end(ctx);
  if (pbs_worker_link_accept(&boot_link, 5000)) {
    /* The context lives in the r-x cave page and is read-only to the worker:
     * there are no in-target progress markers to read back. A successful
     * socket accept is the only proof that the thread started and connected. */
    fprintf(log, "worker boot: worker did not connect (ctx read-only, no markers)\n");
    fprintf(log, "worker boot: SceRemotePlay pid=%d after failed accept\n",
            (int)pbs_find_pid("SceRemotePlay"));
    pbs_worker_link_close(&boot_link);
    return -1;
  }
  fprintf(log, "worker boot: connected, installing hooks\n");
  pbs_inject_set_worker_hooks(hook_call, hook_read, hook_write);
  ctx->worker = 1; /* Even failed setup must retain the detached owner. */
  boot_active = 1;
  int64_t detach_result = -1;
  if (thread_read_failed || !thread ||
      pbs_worker_link_call(&boot_link, (uint64_t)fn_detach,
                           (uint64_t[6]){thread,0,0,0,0,0}, &detach_result, 4000) ||
      detach_result != 0) {
    fprintf(log, "worker boot: pthread_detach failed result=%lld\n",
            (long long)detach_result);
    return -1;
  }
  fprintf(log, "worker boot: pthread resources release automatically on exit\n");
  uint64_t scratch = 0;
  if (pbs_worker_link_scratch(&boot_link, &scratch)) {
    fprintf(log, "worker boot: scratch handshake failed\n");
    return -1;
  }
  ctx->scratch = (intptr_t)scratch;
  ctx->scratch_len = PBS_WORKER_WRITE_MAX;
  fprintf(log, "worker boot: private scratch=%p size=%zu\n",
          (void *)ctx->scratch, ctx->scratch_len);
  return 0;
}

int pbs_ps4_worker_shutdown(void) {
  if (!boot_active) return 0;
  int rc = pbs_worker_link_exit(&boot_link);
  pbs_worker_link_close(&boot_link);
  pbs_inject_set_worker_hooks(NULL, NULL, NULL);
  boot_active = 0;
  return rc;
}
