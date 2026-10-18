// SPDX-License-Identifier: GPL-3.0-or-later
/* Bootstrap orchestration: no ptrace or hardware writes are executed here. */
#include "platform/ps4/worker/boot.h"
#include "platform/ps4/worker/client.h"
#include "platform/ps4/worker/remote.h"
#include "platform/ps4/system/trampoline.h"
#include <assert.h>
#include <string.h>

#define SCRATCH ((intptr_t)0x100000)
#define PRIVATE_SCRATCH UINT64_C(0x200000)
#define THREAD UINT64_C(0x1234)
#define CREATE ((intptr_t)0x300001)
#define DETACH ((intptr_t)0x300002)
static int no_cave, no_export, fail_create, fail_thread_read, fail_accept;
static int fail_detach, fail_scratch;
static int creates, detaches, ends, opens, exits, calls;
static pbs_worker_call_hook call_hook;
static pbs_worker_read_hook read_hook;
static pbs_worker_write_hook write_hook;

intptr_t pbs_resolve_in(pid_t pid, const char *module, const char *symbol) {
  assert(pid == 47);
  if (!strcmp(module, "libSceAvcap.sprx"))
    return !strcmp(symbol, "sceVshAvcapInit") ? 0x800010940 : 0;
  if (strcmp(module, "libkernel_sys.sprx")) return 0;
  assert(strcmp(symbol, "mmap") && strcmp(symbol, "sceKernelMmap"));
  if (!strcmp(symbol, "pthread_create")) return CREATE;
  if (!strcmp(symbol, "pthread_detach")) return no_export ? 0 : DETACH;
  if (!strcmp(symbol, "scePthreadDetach")) return 0;
  return 0x400001;
}
int pbs_inject_read_at(pbs_inject_t *ctx, intptr_t address, void *out, size_t len) {
  assert(ctx->attached);
  if (address == SCRATCH + 0x700) {
    assert(len == sizeof(uint64_t));
    if (fail_thread_read) return -1;
    uint64_t tid = THREAD;
    memcpy(out, &tid, len);
    return 0;
  }
  assert(len == 0x1000);
  if (no_cave) return -1;
  memset(out, 0xcc, len);
  return 0;
}
int pbs_inject_write_at(pbs_inject_t *ctx, intptr_t address, const void *in, size_t len) {
  assert(ctx->attached && address > 0 && in && len);
  return 0;
}
long pbs_inject_call_fast(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]) {
  assert(ctx->attached && fn == CREATE);
  assert(args[0] == SCRATCH + 0x700 && args[1] == 0 && args[2] && args[3]);
  assert(args[0] + sizeof(uint64_t) <= SCRATCH + PBS_PS4_CALL_STATE_OFF);
  ++creates;
  return fail_create ? PBS_CALL_TIMEOUT : 0;
}
void pbs_inject_end(pbs_inject_t *ctx) { assert(ctx->attached); ctx->attached = 0; ++ends; }
pid_t pbs_find_pid(const char *name) { assert(!strcmp(name,"SceRemotePlay")); return 47; }
void pbs_inject_set_worker_hooks(pbs_worker_call_hook c, pbs_worker_read_hook r,
                                 pbs_worker_write_hook w) {
  call_hook = c; read_hook = r; write_hook = w;
}
int pbs_worker_link_open(pbs_worker_link *link, unsigned port) {
  assert(!port); ++opens; link->port = 40000; link->cookie = 42; return 0;
}
int pbs_worker_link_accept(pbs_worker_link *link, unsigned timeout) {
  (void)link; assert(timeout == 5000 && ends == 1); return fail_accept ? -1 : 0;
}
int pbs_worker_link_call(pbs_worker_link *link, uint64_t fn, const uint64_t args[6],
                         int64_t *result, unsigned timeout) {
  (void)link; assert(timeout == 4000 && ends == 1); ++calls;
  if (fn == DETACH) { assert(args[0] == THREAD); ++detaches; *result = fail_detach ? -1 : 0; }
  else *result = 7;
  return 0;
}
int pbs_worker_link_copy(pbs_worker_link *link, uint64_t address, void *out,
                         uint32_t len, unsigned timeout) {
  (void)link; (void)address; (void)out; (void)len; (void)timeout; return 0;
}
int pbs_worker_link_write(pbs_worker_link *link, uint64_t address, const void *in,
                          uint32_t len, unsigned timeout) {
  (void)link; (void)address; (void)in; (void)len; (void)timeout; return 0;
}
int pbs_worker_link_scratch(pbs_worker_link *link, uint64_t *address) {
  (void)link; assert(detaches == 1); *address = PRIVATE_SCRATCH;
  return fail_scratch ? -1 : 0;
}
int pbs_worker_link_exit(pbs_worker_link *link) { (void)link; ++exits; return 0; }
void pbs_worker_link_close(pbs_worker_link *link) { (void)link; }

static void run(int expected, int worker_expected) {
  creates = detaches = ends = opens = exits = calls = 0;
  pbs_inject_t ctx = {.pid=47, .attached=1, .scratch=SCRATCH, .scratch_len=4096,
                      .firmware=pbs_ps4_firmware_find(0x11008001)};
  FILE *log = tmpfile(); assert(log);
  assert(pbs_ps4_worker_boot(&ctx, log) == expected);
  assert(ctx.worker == worker_expected);
  if (!expected) {
    assert(ctx.scratch == PRIVATE_SCRATCH && ctx.scratch_len == 4096 && !ctx.attached);
    assert(detaches == 1 && call_hook && read_hook && write_hook);
    assert(call_hook(0x123, (uint64_t[6]){0}) == 7);
    /* A second bootstrap must not allocate a second worker or listener. */
    ctx.attached = 1;
    assert(pbs_ps4_worker_boot(&ctx, log) < 0 && opens == 1);
    ctx.attached = 0;
  }
  assert(!pbs_ps4_worker_shutdown());
  assert(exits == worker_expected && !call_hook && !read_hook && !write_hook);
  assert(!pbs_ps4_worker_shutdown() && exits == worker_expected);
  fclose(log);
}
int main(void) {
  run(0,1);
  no_cave=1; run(-1,0); no_cave=0;
  no_export=1; run(-1,0); no_export=0;
  fail_create=1; run(-1,0); fail_create=0;
  fail_accept=1; run(-1,0); fail_accept=0;
  fail_thread_read=1; run(-1,1); fail_thread_read=0;
  fail_detach=1; run(-1,1); fail_detach=0;
  fail_scratch=1; run(-1,1); fail_scratch=0;
  puts("Worker bootstrap: SIMD-safe pthread output, detach, private scratch, failure ownership, duplicate refusal, shutdown verified");
  return 0;
}
