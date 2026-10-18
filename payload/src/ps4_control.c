// SPDX-License-Identifier: GPL-3.0-or-later
/* Brief RemotePlay configuration handshake; never trace the frame loop.
 * PS4 11.00 gate sites follow MemDBG (SeregonWar, GPL-3.0-or-later). */
#include "ps4_control.h"
#include "inject.h"
#if PBS_PS4_WORKER_BOOT
#include "ps4_worker_boot.h"
#endif
#include <ps4/kernel.h>
#include <string.h>

#ifndef PBS_PS4_REMOTE_REQUEST
#define PBS_PS4_REMOTE_REQUEST 3
#endif
#if PBS_PS4_REMOTE_REQUEST != 0 && PBS_PS4_REMOTE_REQUEST != 3
#error Unsupported capture request mode
#endif

static int initialized;
static int remote_init_uncertain;
static int capture_request_uncertain;
static pid_t remote_owner_pid;
static uint64_t remote_owner_epoch;
/* libSceAvcap Term decrements its remote reference counter *before* final
 * teardown. After a failed/timeout Term, the reference may already be zero:
 * never send Term again without knowing remote process state. */
static int remote_term_uncertain;
/* Track only patches created by this process. If restoration fails, the next
 * permitted control operation must still restore them even when the gate was
 * already patched on entry. Never revert a patch owned by another process. */
static int restore_pending[PBS_PS4_GATE_COUNT];

/* Retain bootstrap ownership across the worker handoff. last_scratch only
 * records native bootstrap memory; a released worker stack cannot be reused. */
static pbs_inject_t control_ctx;
static int control_ctx_owned;
static intptr_t last_scratch;

pbs_inject_t *pbs_ps4_control_ctx(void) {
  return control_ctx_owned && (control_ctx.attached || control_ctx.worker) ? &control_ctx : NULL;
}

intptr_t pbs_ps4_control_last_scratch(void) { return last_scratch; }

int pbs_ps4_control_ctx_release(void) {
  if (!control_ctx_owned) return 0;
#if PBS_PS4_WORKER_BOOT
  int failed = control_ctx.worker ? pbs_ps4_worker_shutdown() : 0;
  control_ctx.worker = 0;
#else
  int failed = 0;
#endif
  if (control_ctx.attached) pbs_inject_end(&control_ctx);
  control_ctx_owned = 0;
  return failed;
}

int pbs_ps4_control_remote_init_pending(void) { return initialized; }
int pbs_ps4_control_remote_term_uncertain(void) { return remote_term_uncertain; }
int pbs_ps4_control_ownership_uncertain(void) {
  return remote_init_uncertain || capture_request_uncertain || remote_term_uncertain;
}
int pbs_ps4_control_gates_pending(void) { return restore_pending[0] || restore_pending[1]; }

int pbs_ps4_control_restore_gates(FILE *log) {
  const pbs_ps4_firmware *fw = pbs_ps4_firmware_find(kernel_get_fw_version());
  int failed = 0;
  unsigned char current[5];
  if (!fw) return -1;
  for (unsigned i = PBS_PS4_GATE_COUNT; i-- > 0;) {
    if (!restore_pending[i]) continue;
    const pbs_ps4_gate *gate = &fw->gates[i];
    intptr_t address = KERNEL_ADDRESS_IMAGE_BASE + gate->offset;
    if (kernel_copyout(address, current, gate->size)) { failed = -1; continue; }
    if (!memcmp(current, gate->before, gate->size)) {
      restore_pending[i] = 0;
      continue;
    }
    if (memcmp(current, gate->after, gate->size) ||
        kernel_copyin(gate->before, address, gate->size) ||
        kernel_copyout(address, current, gate->size) ||
        memcmp(current, gate->before, gate->size)) {
      failed = -1;
      continue;
    }
    restore_pending[i] = 0;
  }
  if (pbs_ps4_control_gates_pending()) {
    fprintf(log, "WARNING: PS4 debug gate restore pending mask=%u; firmware gates require manual review\n",
            (unsigned)(restore_pending[0] | (restore_pending[1] << 1)));
    return -1;
  }
  return failed;
}

/* Arm the verified profile debug gates without attaching. PT_ATTACH is gated by
 * this kernel policy: without the gates the attach is denied no matter what
 * credentials the payload carries. Safe to call repeatedly; already patched
 * sites are left alone and recorded for pbs_ps4_control_restore_gates(). */
int pbs_ps4_gates_arm(FILE *log) {
  const pbs_ps4_firmware *fw = pbs_ps4_firmware_find(kernel_get_fw_version());
  if (!fw) {
    if (log) fprintf(log, "gates: unsupported firmware\n");
    return -1;
  }
  unsigned char current[5], acmgr[3];
  if (kernel_copyout(KERNEL_ADDRESS_IMAGE_BASE + fw->acmgr_offset, acmgr, sizeof acmgr) ||
      memcmp(acmgr, fw->acmgr_signature, 3)) {
    if (log) fprintf(log, "gates: ACMGR stub mismatch\n");
    return -1;
  }
  for (unsigned i = 0; i < PBS_PS4_GATE_COUNT; ++i) {
    if (kernel_copyout(KERNEL_ADDRESS_IMAGE_BASE + fw->gates[i].offset, current,
                       fw->gates[i].size) ||
        (memcmp(current, fw->gates[i].before, fw->gates[i].size) &&
         memcmp(current, fw->gates[i].after, fw->gates[i].size))) {
      if (log) fprintf(log, "gates: site %u unexpected bytes\n", i);
      return -1;
    }
  }
  for (unsigned i = 0; i < PBS_PS4_GATE_COUNT; ++i) {
    const pbs_ps4_gate *gate = &fw->gates[i];
    const intptr_t address = KERNEL_ADDRESS_IMAGE_BASE + gate->offset;
    if (kernel_copyout(address, current, gate->size)) return -1;
    if (!memcmp(current, gate->after, gate->size)) {
      if (log) fprintf(log, "gates: site %u already armed\n", i);
      continue;
    }
    restore_pending[i] = 1;
    if (kernel_copyin(gate->after, address, gate->size) ||
        kernel_copyout(address, current, gate->size) ||
        memcmp(current, gate->after, gate->size)) {
      if (log) fprintf(log, "gates: site %u patch failed\n", i);
      return -1;
    }
    if (log) fprintf(log, "gates: site %u armed\n", i);
  }
  if (log) fprintf(log, "gates: armed\n");
  return 0;
}

int pbs_ps4_control(FILE *log, int enable, int *request_active) {
  const pbs_ps4_firmware *fw = pbs_ps4_firmware_find(kernel_get_fw_version());
  if (pbs_ps4_control_ownership_uncertain()) {
    fprintf(log, "RemotePlay control refused: previous remote call outcome uncertain (init=%d request=%d term=%d)\n",
            remote_init_uncertain, capture_request_uncertain, remote_term_uncertain);
    pbs_ps4_control_ctx_release();
    return -1;
  }
  if (!fw) {
    pbs_ps4_control_ctx_release();
    return -1;
  }
  int result = -1;
  int remote_init_this_call = 0;
  int active = request_active ? *request_active : 0;
  pbs_inject_t *ctx = pbs_ps4_control_ctx();
  int reused = ctx != NULL;
  if (!ctx) ctx = &control_ctx;
  intptr_t term = 0;
  if (pbs_ps4_gates_arm(log)) goto done;
  if (!reused) {
    if (pbs_inject_begin_preferred("SceRemotePlay", ctx, last_scratch)) goto done;
    control_ctx_owned = 1;
  }
  ctx->firmware = fw;
  /* A worker stack may be reclaimed after EXIT; never reuse it for ptrace. */
  if (!ctx->worker) last_scratch = ctx->scratch;
  uint64_t current_epoch = pbs_process_birth_epoch(ctx->pid);
  if (!current_epoch) {
    /* Once we own a remote reference, inability to distinguish this PID's
     * birth is also unsafe: never retry a blind SetStaticInfo/Term. */
    if (initialized || active) capture_request_uncertain = 1;
    fprintf(log, "RemotePlay process birth unavailable pid=%d; refusing uncertain global capture ownership\n", (int)ctx->pid);
    goto done;
  }
  if ((initialized || active) &&
      (remote_owner_pid != ctx->pid || remote_owner_epoch != current_epoch)) {
    capture_request_uncertain = 1;
    fprintf(log, "WARNING: RemotePlay process replaced owner=(%d,%llu) current=(%d,%llu); refusing to touch replacement\n",
            (int)remote_owner_pid, (unsigned long long)remote_owner_epoch,
            (int)ctx->pid, (unsigned long long)current_epoch);
    goto done;
  }
  intptr_t init = pbs_resolve_in(ctx->pid, "libSceAvcap.sprx", "sceVshAvcapInit");
  term = pbs_resolve_in(ctx->pid, "libSceAvcap.sprx", "sceVshAvcapTerm");
  intptr_t set = pbs_resolve_in(ctx->pid, "libSceAvcap.sprx", "sceVshAvcapSetStaticInfo");
  if (!init || !term || !set) goto done;
  /* No kernel_dynlib_obj read here anymore: its firmware object layout differs
   * from the SDK header, the libkernel range was never obtained on hardware,
   * and walking it preceded two console kernel panics (17/10/2026). */
#ifdef PBS_PS4
  /* Entry for the FP-preserving trampoline in validated import padding.
   * The injector restores every changed padding slot before returning;
   * import JMPs remain untouched. A current-RIP trap is not a safe fallback. */
  ctx->trap = pbs_ps4_firmware_trap(fw, init);
  fprintf(log, "RemotePlay call entry=%p (libSceAvcap padding; tracee x87/SSE/AVX preservation)\n", (void *)ctx->trap);
  fprintf(log, "PS4 ptrace uses inherited loader credentials; no authid/caps writes\n");
#endif
  if (enable && !initialized) {
    long rc = pbs_inject_call_fast(ctx, init, (uint64_t[6]){0});
    if (rc == LONG_MIN) {
      /* The target may already have incremented its Avcap refcount. */
      remote_init_uncertain = 1;
      remote_owner_pid = ctx->pid;
      remote_owner_epoch = current_epoch;
      fprintf(log, "WARNING: RemotePlay Init timeout, refcount ownership unknown; no blind Term\n");
      fprintf(log, "RemotePlay Init call failed stage=%s errno=%d\n",
              ctx->call_stage ? ctx->call_stage : "unavailable", ctx->call_errno);
      goto done;
    }
    if ((int32_t)rc < 0) goto done;
    initialized = 1;
    remote_owner_pid = ctx->pid;
    remote_owner_epoch = current_epoch;
    remote_init_this_call = 1;
  }
  long rc = 0;
  if ((enable && PBS_PS4_REMOTE_REQUEST) || active) {
    uint32_t flags = enable ? PBS_PS4_REMOTE_REQUEST : 0;
    if (pbs_inject_write(ctx, &flags, sizeof flags)) goto done;
    rc = pbs_inject_call_fast(ctx, set, (uint64_t[6]){0x15, ctx->scratch, 4});
    fprintf(log, "RemotePlay capture request %u -> %08lx\n", flags, (unsigned long)rc);
    if (rc == LONG_MIN || (int32_t)rc < 0) {
      /* The setter may already have applied part of the request. A global
       * SetStaticInfo(21,0) rollback would also clear someone else's capture
       * request if native RemotePlay acquired it concurrently. */
      capture_request_uncertain = 1;
      fprintf(log, "WARNING: RemotePlay SetStaticInfo outcome uncertain; NOT clearing unowned global request\n");
      goto done;
    }
    active = !!enable;
  }
  if (enable && !PBS_PS4_REMOTE_REQUEST)
    fprintf(log, "Generic capture: RemotePlay-specific request not acquired\n");
  result = 0;
  if (!enable && initialized) {
    rc = pbs_inject_call_fast(ctx, term, (uint64_t[6]){0});
    initialized = 0;
    if (rc == LONG_MIN || (int32_t)rc < 0) {
      remote_term_uncertain = 1;
      fprintf(log, "WARNING: remote Avcap Term failed/unknown (%08lx); no blind Term retry\n",
              (unsigned long)rc);
      result = -1;
    } else {
      remote_owner_pid = 0;
      remote_owner_epoch = 0;
    }
  }
done:
  /* Before the successful setter return there is no confirmed request to
   * undo. Never issue an unsolicited SetStaticInfo(21,0) after a timeout or
   * negative setter result: a competing RP session may now own that state. */
  if (result && enable && remote_init_this_call && ctx->attached) {
    if (!capture_request_uncertain) {
      long term_rc = pbs_inject_call_fast(ctx, term, (uint64_t[6]){0});
      fprintf(log, "RemotePlay failed enable Term -> %08lx\n", (unsigned long)term_rc);
      initialized = 0;
      if (term_rc == LONG_MIN || (int32_t)term_rc < 0) remote_term_uncertain = 1;
      else { remote_owner_pid = 0; remote_owner_epoch = 0; }
    } else {
      fprintf(log, "WARNING: skip global rollback and remote Term while request ownership is uncertain\n");
    }
  }
  /* Single-attach capture: keep the control attachment and the debug gates
   * active for the whole capture session. Restoring the gates before the
   * capture made later PT_IO writes fail; keeping them until after
   * Stop/Close/SetStaticInfo(0)/Term is required for the injected calls. */
#if PBS_PS4_REMOTE_AVCAP
  int retain = enable && result == 0 && !capture_request_uncertain && (ctx->attached || ctx->worker);
  if (retain) {
    if (pbs_inject_ensure_stopped(ctx) || pbs_inject_revalidate_scratch(ctx)) {
      fprintf(log, "WARNING: retained context unsafe after control (stop/scratch); detaching\n");
      retain = 0;
      result = -1;
    } else {
      fprintf(log, "RemotePlay debug gates retained for the capture session\n");
    }
  }
  if (!retain) {
    if (pbs_ps4_control_restore_gates(log)) result = -1;
  }
#else
  int retain = 0;
  if (pbs_ps4_control_restore_gates(log)) result = -1;
#endif
  if (!retain && control_ctx_owned) {
    if (pbs_ps4_control_ctx_release()) result = -1;
  }
  if (request_active) *request_active = active;
  if (result < 0 && active)
    fprintf(log, "WARNING: RemotePlay capture request may still be active after failed control operation\n");
  fprintf(log, "RemotePlay control phase complete, restore/result=%d retain=%d\n",
          result, retain);
  return result;
}
