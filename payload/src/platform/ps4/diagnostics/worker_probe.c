// SPDX-License-Identifier: GPL-3.0-or-later
/* Transport-only probe: no Avcap function is called. The FW11 debug gates are
 * patched (kernel writes only) because PT_ATTACH is policy-gated by them. The
 * injected-call trampoline is taken from libSceAvcap import padding, which only
 * requires that module to be mapped, not initialized. */
#include "platform/ps4/diagnostics/worker_probe.h"
#include "platform/ps4/worker/boot.h"
#include "platform/ps4/system/control.h"
#include <string.h>

/* Every injected call needs a return trap inside executable memory.
 * pb_call_fast refuses a context with trap == 0 ("ps4-trampoline-unavailable",
 * ENOTSUP) *before* it touches the target, so an unarmed probe reports
 * PBS_CALL_TIMEOUT without ever calling anything. That is what the 18/10 runs
 * showed. Arm the same FW11 padding the control phase uses
 * (sceVshAvcapInit - 0x940 + 0xbe: twenty ten-byte 0xCC slots every sixteen
 * bytes). The slots are validated and restored by the injector on every call. */
static int probe_arm_trap(pbs_inject_t *ctx, FILE *log) {
  intptr_t init =
      pbs_resolve_in(ctx->pid, "libSceAvcap.sprx", "sceVshAvcapInit");
  if (!init) {
    fprintf(log, "worker probe: trap unavailable: "
                 "libSceAvcap.sprx!sceVshAvcapInit unresolved\n");
    return -1;
  }
  ctx->trap = pbs_ps4_firmware_trap(ctx->firmware, init);
  fprintf(log, "worker probe: trap entry=%p (libSceAvcap FW11 padding, init=%p)\n",
          (void *)ctx->trap, (void *)init);
  return 0;
}

/* A failed fast call is not necessarily a timeout: report the stage so a
 * configuration failure (no trap, no scratch) is distinguishable from a callee
 * that never returned. Calls routed through the worker hook do not update the
 * stage, hence the explicit "worker-hook" fallback. */
static long probe_call(pbs_inject_t *ctx, FILE *log, const char *label,
                       intptr_t fn, const uint64_t args[6]) {
  ctx->call_stage = NULL;
  ctx->call_errno = 0;
  long rc = pbs_inject_call_fast(ctx, fn, args);
  if (rc == PBS_CALL_TIMEOUT)
    fprintf(log, "worker probe: %s failed stage=%s errno=%d\n", label,
            ctx->call_stage ? ctx->call_stage : "worker-hook",
            ctx->call_errno);
  return rc;
}

/* Detach if still attached, then restore only the gate sites this process
 * patched; a site already armed by the loader is left untouched. */
static int probe_finish(pbs_inject_t *ctx, FILE *log, int rc) {
  if (ctx->attached) pbs_inject_end(ctx);
  if (pbs_ps4_control_restore_gates(log)) rc = -1;
  return rc;
}

int pbs_ps4_worker_probe(pbs_inject_t *ctx, FILE *log) {
  if (!ctx || !log || ctx->attached) return -1;
  /* PT_ATTACH is gated by kernel policy: arm the FW11 debug gates first
   * (kernel writes, no attachment needed). The identity adoption happens
   * inside pbs_inject_begin_preferred. */
  if (pbs_ps4_gates_arm(log)) {
    fprintf(log, "worker probe: gate arming failed\n");
    return probe_finish(ctx, log, -1);
  }
  if (pbs_inject_begin_preferred("SceRemotePlay", ctx, 0)) {
    fprintf(log, "worker probe: attach failed\n");
    return probe_finish(ctx, log, -1);
  }
  const pid_t expected_pid = ctx->pid;
  /* Post-attach context telemetry: a zero LWP or a zero trap makes every
   * injected call fail for configuration reasons, not for concurrency. */
  fprintf(log,
          "worker probe: context pid=%d lwp=%d scratch=%p len=%zu trap=%p running=%d\n",
          (int)ctx->pid, (int)ctx->lwp, (void *)ctx->scratch, ctx->scratch_len,
          (void *)ctx->trap, ctx->running);
  const int stopped = pbs_inject_ensure_stopped(ctx);
  fprintf(log, "worker probe: ensure_stopped=%d lwp=%d trap=%p\n", stopped,
          (int)ctx->lwp, (void *)ctx->trap);
  if (stopped) {
    fprintf(log, "worker probe: target not stopped; aborting before any call\n");
    return probe_finish(ctx, log, -1);
  }
  if (probe_arm_trap(ctx, log)) return probe_finish(ctx, log, -1);
  const intptr_t getpid_fn =
      pbs_resolve_in(ctx->pid, "libkernel_sys.sprx", "getpid");
  if (!getpid_fn) {
    fprintf(log, "worker probe: getpid unresolved\n");
    return probe_finish(ctx, log, -1);
  }
  /* Decisive pre-boot call: a lock-free libkernel export over ptrace. It must
   * return the target PID; the trap is armed above, so this really executes.
   * If it fails, the boot's mmap/pthread_create cannot work either. */
  const long pre =
      probe_call(ctx, log, "pre-boot getpid", getpid_fn, (uint64_t[6]){0});
  fprintf(log, "worker probe: pre-boot getpid rc=%016llx expected=%d match=%d\n",
          (unsigned long long)pre, (int)expected_pid,
          pre == (long)expected_pid);
  /* No timeout and no foreign PID is acceptable here: either one means the
   * borrowed-thread call path is not trustworthy, and the boot relies on it. */
  if (pre != (long)expected_pid) return probe_finish(ctx, log, -1);
  if (pbs_ps4_worker_boot(ctx, log)) {
    fprintf(log, "worker probe: boot failed\n");
    return probe_finish(ctx, log, -1);
  }
  /* Hooks now route this call through the socket, with the daemon running. */
  const long actual_pid =
      probe_call(ctx, log, "socket getpid", getpid_fn, (uint64_t[6]){0});
  int rc = actual_pid == (long)expected_pid ? 0 : -1;
  fprintf(log, "worker probe: getpid expected=%d actual=%lld result=%d\n",
          (int)expected_pid, (long long)actual_pid, rc);
  if (pbs_ps4_worker_shutdown()) rc = -1;
  fprintf(log, "worker probe: result=%d (target mapping retained)\n", rc);
  return probe_finish(ctx, log, rc);
}
