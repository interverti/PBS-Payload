// SPDX-License-Identifier: GPL-3.0-or-later
/* Link the transport-only probe without any Avcap/control implementation.
 * Build with -DPBS_PS4: the probe arms the FW11 trampoline layout defined in
 * inject.h, and the stubs enforce that ordering. */
#include "ps4_worker_probe.h"
#include "ps4_worker_boot.h"
#include <assert.h>
#include <string.h>

#define STUB_GETPID ((intptr_t)0x100000001)
#define STUB_AVCAP_INIT ((intptr_t)0x200000940)
#define STUB_TRAP ((intptr_t)0x2000000be) /* init - 0x940 + 0xbe */

static int fail_attach, fail_getpid_resolve, fail_trap_resolve, fail_boot;
static int fail_exit, fail_gates, fail_restore;
static int fail_call = -1; /* 0 = pre-boot call, 1 = socket call */
static int wrong_pid, wrong_socket_pid;
static int calls, ends, boots, exits, arms, restores;

int pbs_ps4_gates_arm(FILE *log) {
  assert(log);
  ++arms;
  return fail_gates ? -1 : 0;
}
int pbs_ps4_control_restore_gates(FILE *log) {
  assert(log);
  ++restores;
  return fail_restore ? -1 : 0;
}
int pbs_inject_begin_preferred(const char *name, pbs_inject_t *ctx,
                               intptr_t preferred) {
  assert(!strcmp(name, "SceRemotePlay") && !preferred);
  if (fail_attach) return -1;
  ctx->firmware = pbs_ps4_firmware_find(0x11008001u);
  ctx->attached = 1;
  ctx->pid = 47;
  ctx->lwp = 48;
  ctx->scratch = (intptr_t)0x7f0000000000;
  ctx->scratch_len = 0x1000;
  return 0;
}
intptr_t pbs_resolve_in(pid_t pid, const char *module, const char *symbol) {
  assert(pid == 47);
  if (!strcmp(module, "libkernel_sys.sprx") && !strcmp(symbol, "getpid"))
    return fail_getpid_resolve ? 0 : STUB_GETPID;
  if (!strcmp(module, "libSceAvcap.sprx") && !strcmp(symbol, "sceVshAvcapInit"))
    return fail_trap_resolve ? 0 : STUB_AVCAP_INIT;
  assert(!"unexpected symbol");
  return 0;
}
int pbs_inject_ensure_stopped(pbs_inject_t *ctx) {
  assert(ctx->attached);
  return 0;
}
void pbs_inject_end(pbs_inject_t *ctx) {
  assert(ctx->attached);
  ctx->attached = 0;
  ++ends;
}
int pbs_ps4_worker_boot(pbs_inject_t *ctx, FILE *log) {
  assert(log && ctx->attached && ctx->trap == STUB_TRAP);
  ++boots;
  if (fail_boot == 1) return -1;
  ctx->attached = 0;
  return fail_boot == 2 ? -1 : 0;
}
long pbs_inject_call_fast(pbs_inject_t *ctx, intptr_t fn, const uint64_t a[6]) {
  /* Regression for the 18/10 runs: with trap == 0 the injector rejects the
   * call before executing it, so the probe must never reach this point
   * without an armed trampoline. */
  assert(ctx->trap == STUB_TRAP && fn == STUB_GETPID);
  for (int i = 0; i < 6; ++i) assert(a[i] == 0);
  const int index = calls++;
  /* Call 0 runs over ptrace while still attached; call 1 goes through the
   * worker socket after the boot detached. */
  assert(ctx->attached == (index == 0));
  if (fail_call == index) return PBS_CALL_TIMEOUT;
  if (index == 0) return wrong_pid ? 46 : 47;
  return wrong_socket_pid ? 46 : 47;
}
int pbs_ps4_worker_shutdown(void) {
  ++exits;
  return fail_exit ? -1 : 0;
}

static void run(int expected, int want_arms, int want_boots, int want_calls,
                int want_exits, int want_ends, int want_restores) {
  calls = ends = boots = exits = arms = restores = 0;
  pbs_inject_t ctx = {0};
  FILE *log = tmpfile();
  assert(log);
  assert(pbs_ps4_worker_probe(&ctx, log) == expected);
  assert(arms == want_arms && boots == want_boots && calls == want_calls);
  assert(exits == want_exits && ends == want_ends && restores == want_restores);
  assert(!ctx.attached);
  fclose(log);
}

int main(void) {
  /* Success: gates, attach, trap, ptrace getpid, boot, socket getpid, exit. */
  run(0, 1, 1, 2, 1, 0, 1);
  fail_gates = 1; run(-1, 1, 0, 0, 0, 0, 1); fail_gates = 0;
  fail_attach = 1; run(-1, 1, 0, 0, 0, 0, 1); fail_attach = 0;
  fail_trap_resolve = 1; run(-1, 1, 0, 0, 0, 1, 1); fail_trap_resolve = 0;
  fail_getpid_resolve = 1; run(-1, 1, 0, 0, 0, 1, 1); fail_getpid_resolve = 0;
  /* Pre-boot call did not return: no boot, detach and gate restore. */
  fail_call = 0; run(-1, 1, 0, 1, 0, 1, 1); fail_call = -1;
  /* Pre-boot call returned a different PID: the trap path is not trustworthy. */
  wrong_pid = 1; run(-1, 1, 0, 1, 0, 1, 1); wrong_pid = 0;
  fail_boot = 1; run(-1, 1, 1, 1, 0, 1, 1);
  fail_boot = 2; run(-1, 1, 1, 1, 0, 0, 1); fail_boot = 0;
  /* Socket call failed or returned another PID: shutdown still runs. */
  fail_call = 1; run(-1, 1, 1, 2, 1, 0, 1); fail_call = -1;
  wrong_socket_pid = 1; run(-1, 1, 1, 2, 1, 0, 1); wrong_socket_pid = 0;
  fail_exit = 1; run(-1, 1, 1, 2, 1, 0, 1); fail_exit = 0;
  fail_restore = 1; run(-1, 1, 1, 2, 1, 0, 1); fail_restore = 0;
  puts("worker probe: trampoline armed before ptrace, getpid pre-boot and over "
       "the worker socket, gate restore on every path");
  return 0;
}
