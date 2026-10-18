// SPDX-License-Identifier: GPL-3.0-or-later
/* Host-only service simulation for PS4 gate restore failure *after* a
 * successful SetStaticInfo(21)=3. Ensure cleanup can find ownership and
 * restore both local gate patches on the next authorized operation. */
#include "platform/ps4/system/control.h"
#include "common/process/inject.h"
#include <ps4/kernel.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned char first[2] = {0x77, 0x1c};
static unsigned char second[5] = {0x48, 0x8b, 0x41, 0x08, 0x44};
static const unsigned char first_native[2] = {0x77, 0x1c};
static const unsigned char second_native[5] = {0x48, 0x8b, 0x41, 0x08, 0x44};
#if PBS_PS4_REMOTE_AVCAP
static const unsigned char first_patched[2] = {0xeb, 0x1c};
static const unsigned char second_patched[5] = {0xe9, 0x7c, 0x02, 0, 0};
#endif
static unsigned char acmgr[] = {0xb0, 1, 0xc3};
static unsigned last_scratch, remote_flags, set_calls, term_calls, init_calls;
static int fail_restore_once = 1;
static int fail_remote_term_once;
static unsigned kernel_reads, kernel_writes;
#if PBS_PS4_WORKER_BOOT
static int worker_shutdown_calls, expected_shutdown_calls;
int pbs_ps4_worker_shutdown(void) { ++worker_shutdown_calls; return 0; }
#endif
static int fail_init_timeout_once, fail_enable_set_timeout_once, fail_enable_set_error_once;
static pid_t current_remote_pid = 48;
static uint64_t current_remote_birth = 1234000000;
uint64_t pbs_process_birth_epoch(pid_t pid) {
  return pid == current_remote_pid ? current_remote_birth : 0;
}

static unsigned char *resolve_address(intptr_t address, size_t len) {
  switch (address - KERNEL_ADDRESS_IMAGE_BASE) {
    case 0x3d0de0: return len == sizeof acmgr ? acmgr : NULL;
    case 0x384285: return len == sizeof first ? first : NULL;
    case 0x384771: return len == sizeof second ? second : NULL;
    default: return NULL;
  }
}
int kernel_copyout(intptr_t address, void *dest, size_t len) {
  ++kernel_reads;
  unsigned char *src = resolve_address(address, len);
  if (!src) return -1;
  memcpy(dest, src, len);
  return 0;
}
int kernel_copyin(const void *src, intptr_t address, size_t len) {
  ++kernel_writes;
  unsigned char *dest = resolve_address(address, len);
  if (!dest) return -1;
  if (fail_restore_once && len == sizeof second && !memcmp(src, second_native, sizeof second_native)) {
    fail_restore_once = 0;
    return -1;
  }
  memcpy(dest, src, len);
  return 0;
}
/* No libkernel range in the stub environment: LWP preference stays disabled. */
int kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle) {
  (void)pid; (void)basename; (void)handle;
  return -1;
}
int kernel_dynlib_obj(pid_t pid, uint32_t handle, dynlib_obj_t *obj) {
  (void)pid; (void)handle; (void)obj;
  return -1;
}
int pbs_inject_begin(const char *name, pbs_inject_t *ctx) {
  assert(!strcmp(name, "SceRemotePlay"));
  ctx->pid = current_remote_pid; ctx->scratch = 0x10000; ctx->attached = 1;
  return 0;
}
int pbs_inject_begin_preferred(const char *name, pbs_inject_t *ctx,
                               intptr_t preferred_page) {
  (void)preferred_page;
  return pbs_inject_begin(name, ctx);
}
int pbs_inject_ensure_stopped(pbs_inject_t *ctx) { (void)ctx; return 0; }
int pbs_inject_revalidate_scratch(pbs_inject_t *ctx) { (void)ctx; return 0; }
int pbs_inject_resume(pbs_inject_t *ctx) { (void)ctx; return 0; }
int pbs_inject_target_rsp(pbs_inject_t *ctx, intptr_t *rsp) {
  (void)ctx; *rsp = 0; return -1;
}
intptr_t pbs_resolve_in(pid_t pid, const char *lib, const char *symbol) {
  assert(pid == current_remote_pid && !strcmp(lib, "libSceAvcap.sprx"));
  if (!strcmp(symbol, "sceVshAvcapInit")) return 1;
  if (!strcmp(symbol, "sceVshAvcapTerm")) return 2;
  if (!strcmp(symbol, "sceVshAvcapSetStaticInfo")) return 3;
  return 0;
}
int pbs_inject_write(pbs_inject_t *ctx, const void *src, size_t len) {
  assert((ctx->attached || ctx->worker) && len == 4);
  uint32_t flags;
  memcpy(&flags, src, sizeof flags);
  last_scratch = (unsigned char)flags;
  return 0;
}
int pbs_inject_write_at(pbs_inject_t *ctx, intptr_t addr, const void *in, size_t len) {
  (void)ctx; (void)addr; (void)in; (void)len;
  return 0;
}
int pbs_inject_read_at(pbs_inject_t *ctx, intptr_t addr, void *out, size_t len) {
  (void)ctx; (void)addr;
  if (out) memset(out, 0, len);
  return 0;
}
long pbs_inject_call_fast(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]) {
  assert(ctx->attached || ctx->worker);
  if (fn == 1 || fn == 2) {
    if (fn == 1) {
      init_calls++;
      if (fail_init_timeout_once) { fail_init_timeout_once = 0; return LONG_MIN; }
    }
    if (fn == 2) {
      term_calls++;
      if (fail_remote_term_once) { fail_remote_term_once = 0; return (long)(int32_t)0x80a70001; }
    }
    return 0;
  }
  assert(fn == 3 && args[0] == 0x15 && args[1] == (uint64_t)ctx->scratch && args[2] == 4);
  remote_flags = last_scratch;
  set_calls++;
  if (last_scratch == 3 && fail_enable_set_timeout_once) {
    fail_enable_set_timeout_once = 0;
    return LONG_MIN;
  }
  if (last_scratch == 3 && fail_enable_set_error_once) {
    fail_enable_set_error_once = 0;
    return (long)(int32_t)0x809a0082;
  }
  return 0;
}
void pbs_inject_end(pbs_inject_t *ctx) { ctx->attached = 0; }

int main(int argc, char **argv) {
  FILE *log = tmpfile();
  assert(log);
  int active = 0;
  if (argc == 2 && !strcmp(argv[1], "generic-profile")) {
    fail_restore_once = 0;
    assert(pbs_ps4_control(log, 1, &active) == 0);
    assert(!active && init_calls == 1 && !set_calls);
    assert(pbs_ps4_control_ctx() && pbs_ps4_control_remote_init_pending());
    assert(pbs_ps4_control(log, 0, &active) == 0);
    assert(!active && term_calls == 1 && !set_calls);
    assert(!pbs_ps4_control_remote_init_pending() && !pbs_ps4_control_gates_pending());
    fclose(log);
    puts("Generic capture: owned Init/Term without a RemotePlay request or global clear");
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "unsupported-firmware")) {
    assert(pbs_ps4_gates_arm(log) < 0);
    assert(pbs_ps4_control(log, 1, &active) < 0);
    assert(!kernel_reads && !kernel_writes && !init_calls && !set_calls);
    assert(!active && !pbs_ps4_control_ctx());
    fclose(log);
    puts("Unsupported firmware: no kernel reads/writes, attach or remote calls");
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "gate-mismatch")) {
    first[0] = 0x42;
    assert(pbs_ps4_gates_arm(log) < 0);
    assert(pbs_ps4_control(log, 1, &active) < 0);
    assert(!kernel_writes && !init_calls && !set_calls);
    fclose(log);
    puts("Gate signature mismatch: no patch/attach or remote calls");
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "replaced-process")) {
    fail_restore_once = 0;
    assert(pbs_ps4_control(log, 1, &active) == 0);
    assert(active && init_calls == 1 && set_calls == 1);
    /* Same PID reused but different process start time: no Set/Term. */
    current_remote_birth++;
    assert(pbs_ps4_control(log, 0, &active) < 0);
    assert(active && set_calls == 1 && term_calls == 0);
    assert(pbs_ps4_control_ownership_uncertain());
    assert(!pbs_ps4_control_gates_pending());
    fclose(log);
    puts("PS4 SceRemotePlay process replacement detected with no remote mutation");
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "identity-unavailable")) {
    fail_restore_once = 0;
    assert(pbs_ps4_control(log, 1, &active) == 0);
    assert(active && set_calls == 1 && term_calls == 0);
    current_remote_birth = 0;
    assert(pbs_ps4_control(log, 0, &active) < 0);
    assert(active && set_calls == 1 && term_calls == 0);
    assert(pbs_ps4_control_ownership_uncertain());
    assert(!pbs_ps4_control_gates_pending());
    fclose(log);
    puts("PS4 unobservable process birth prevents mutation of unknown RemotePlay owner");
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "init-timeout")) {
    fail_restore_once = 0;
    fail_init_timeout_once = 1;
    assert(pbs_ps4_control(log, 1, &active) < 0);
    assert(init_calls == 1 && !term_calls && !set_calls);
    assert(!active && pbs_ps4_control_ownership_uncertain());
    assert(!pbs_ps4_control_gates_pending());
    assert(pbs_ps4_control(log, 0, &active) < 0 && !term_calls);
    fclose(log);
    puts("PS4 remote Init timeout quarantined with no unsafe Term/retry");
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "set-timeout")) {
    fail_restore_once = 0;
    fail_enable_set_timeout_once = 1;
    assert(pbs_ps4_control(log, 1, &active) < 0);
    assert(init_calls == 1 && remote_flags == 3 && set_calls == 1);
    assert(!active && !term_calls && pbs_ps4_control_ownership_uncertain());
    assert(!pbs_ps4_control_gates_pending());
    assert(pbs_ps4_control(log, 0, &active) < 0 && !term_calls);
    fclose(log);
    puts("PS4 uncertain global capture request quarantined without global rollback");
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "set-negative")) {
    fail_restore_once = 0;
    fail_enable_set_error_once = 1;
    assert(pbs_ps4_control(log, 1, &active) < 0);
    assert(remote_flags == 3 && set_calls == 1);
    assert(init_calls == 1 && term_calls == 0);
    assert(!active && pbs_ps4_control_ownership_uncertain());
    assert(!pbs_ps4_control_gates_pending());
    fclose(log);
    puts("PS4 negative SetStaticInfo quarantined without clearing competing owner");
    return 0;
  }
#if PBS_PS4_REMOTE_AVCAP
  /* Retained-session mode: both gates stay patched for the capture backend.
   * A failure during the final control(0) restore must leave them pending
   * until the explicit gate-only recovery. */
  assert(pbs_ps4_control(log, 1, &active) == 0);
  assert(active == 1 && remote_flags == 3 && set_calls == 1);
  assert(!memcmp(first, first_patched, sizeof first_patched));
  assert(!memcmp(second, second_patched, sizeof second_patched));
  assert(pbs_ps4_control_gates_pending());
#if PBS_PS4_WORKER_BOOT
  {
  pbs_inject_t *shared = pbs_ps4_control_ctx();
  assert(shared && shared->attached);
  shared->attached = 0;
  shared->worker = 1;
  shared->scratch = 0x20000; /* Worker-owned scratch, not the old native stack. */
  assert(pbs_ps4_control_ctx() == shared);
  }
#endif
  fail_restore_once = 1;
  assert(pbs_ps4_control(log, 0, &active) < 0);
#if PBS_PS4_WORKER_BOOT
  assert(worker_shutdown_calls == ++expected_shutdown_calls);
  assert(pbs_ps4_control_last_scratch() == 0x10000);
  assert(pbs_ps4_control_ctx() == NULL);
#endif
  assert(active == 0 && remote_flags == 0 && term_calls == 1);
  assert(pbs_ps4_control_gates_pending());
  assert(pbs_ps4_control_restore_gates(log) == 0);
  assert(!pbs_ps4_control_gates_pending());
#else
  /* The first successful remote set is followed by one failed kernel gate
   * restoration. Caller must know the RP request remains in effect. */
  assert(pbs_ps4_control(log, 1, &active) < 0);
  assert(active == 1 && remote_flags == 3 && set_calls == 1);
  assert(!memcmp(first, first_native, sizeof first_native));
  assert(memcmp(second, second_native, sizeof second_native));
  /* New control invocation owns the prior modified gate; cleanup must clear
   * the request and restore the pending gate, without touching unknown ones. */
  assert(pbs_ps4_control(log, 0, &active) == 0);
  assert(active == 0 && remote_flags == 0 && term_calls == 1);
  assert(!memcmp(first, first_native, sizeof first_native));
  assert(!memcmp(second, second_native, sizeof second_native));
#endif

  /* A gate restore may fail AFTER successful remote release. Cleanup must
   * restore our owned gate without changing static request or Term again. */
  assert(pbs_ps4_control(log, 1, &active) == 0);
  assert(active == 1);
#if PBS_PS4_WORKER_BOOT
  {
  pbs_inject_t *shared = pbs_ps4_control_ctx();
  assert(shared && shared->attached);
  shared->attached = 0;
  shared->worker = 1;
  shared->scratch = 0x20000; /* Worker-owned scratch, not the old native stack. */
  assert(pbs_ps4_control_ctx() == shared);
  }
#endif
  fail_restore_once = 1;
  assert(pbs_ps4_control(log, 0, &active) < 0);
#if PBS_PS4_WORKER_BOOT
  assert(worker_shutdown_calls == ++expected_shutdown_calls);
  assert(pbs_ps4_control_last_scratch() == 0x10000);
  assert(pbs_ps4_control_ctx() == NULL);
#endif
  assert(active == 0 && remote_flags == 0 && pbs_ps4_control_gates_pending());
  unsigned sets_before_cleanup = set_calls, terms_before_cleanup = term_calls;
  assert(pbs_ps4_control_restore_gates(log) == 0);
  assert(!pbs_ps4_control_gates_pending());
  assert(set_calls == sets_before_cleanup && term_calls == terms_before_cleanup);
  assert(!memcmp(first, first_native, sizeof first_native));
  assert(!memcmp(second, second_native, sizeof second_native));

  /* Native Term may already have decremented its reference counter when it
   * reports failure; a retry must not decrement again. */
  assert(pbs_ps4_control(log, 1, &active) == 0);
  fail_remote_term_once = 1;
  assert(pbs_ps4_control(log, 0, &active) < 0);
  assert(active == 0 && pbs_ps4_control_remote_term_uncertain());
  assert(!pbs_ps4_control_remote_init_pending());
  assert(pbs_ps4_control(log, 1, &active) < 0);
  assert(term_calls == terms_before_cleanup + 1);
  fclose(log);
  puts("PS4 capture rollback, independent gate restore and Term refusal verified");
  return 0;
}
