// SPDX-License-Identifier: GPL-3.0-or-later
/* PS4 Avcap backend executed inside SceRemotePlay.
 *
 * FW11 classifies the client of the capture service. A standalone payload
 * that opens the local libSceAvcap client receives 0x80a70008 before an
 * encoder is assigned. Native RemotePlay performs Init/SetStaticInfo/Open/
 * GetInfo/Start/Read/Stop/Close in its own process, so this backend attaches
 * to SceRemotePlay, executes the same ABI calls there, and copies the
 * produced Annex B access units into the payload for HTTP/RTP.
 *
 * Frame transport uses the Avcap client's already-mapped ring buffer. The
 * internal dequeue helper returns the payload pointer and length; PBS copies
 * that payload with PT_IO and then consumes the ring entry. This avoids
 * calling malloc/mmap in a stopped SceRemotePlay thread (both can block on
 * heap/VM state held by other stopped threads).
 *
 * No signal handler, global setter or private ioctl is used here. */
#ifndef PBS_PS4_LOW_MEMORY
#define PBS_PS4_LOW_MEMORY 0
#endif
#include "platform/ps4/capture/remote.h"
#include "platform/ps4/worker/boot.h"
#include "common/process/inject.h"
#include "platform/ps4/system/control.h"
#include "platform/ps4/capture/request.h"
#include "platform/ps4/capture/ring.h"
#if PBS_PS4_ADMISSION_PROBE || PBS_PS4_REMOTE_1080
#include "platform/ps4/diagnostics/admission.h"
#endif
#if PBS_PS4_LATCH_BYPASS || PBS_PS4_BUDGET_UNLOCK || PBS_PS4_NATIVE_SOURCE
#include "platform/ps4/diagnostics/latch_patch.h"
#endif

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PBS_PS4R_REQUEST_OFF 0x000u
#define PBS_PS4R_HANDLE_OFF  0x200u
#define PBS_PS4R_INFO_OFF    0x300u
#define PBS_PS4R_PACKET_OFF  0x680u
#define PBS_PS4R_DESC_OFF    0x500u
#define PBS_PS4R_STATIC_OFF  0x700u
#define PBS_PS4R_STATUS_OFF  0x780u
#define PBS_PS4R_INFO_SIZE   0x100u
#define PBS_PS4R_META_SIZE   0x40u

#ifndef PBS_PS4_REMOTE_RING_COPY
#define PBS_PS4_REMOTE_RING_COPY 1
#endif
/* Isolated comparison with PS5's blocking ReadVideo. Let other native
 * RemotePlay threads run during waits without SIGSTOP or LWP reselection.
 * Experimental until physical display stability has been verified. */
#ifndef PBS_PS4_REMOTE_IDLE_RUN
#define PBS_PS4_REMOTE_IDLE_RUN 0
#endif

/* Calling SceRemotePlay's own ensure-open with a session object located by
 * memory scan is NOT proven safe on FW 11.00: the two 17/10 kernel panics
 * make it an explicit opt-in experiment only. The direct Avcap path is the
 * production route. */
#ifndef PBS_PS4_NATIVE_ENSURE_OPEN
#define PBS_PS4_NATIVE_ENSURE_OPEN 0
#endif

/* Native remoteplay.elf calls this private Avcap export with a 0x10-byte
 * surface descriptor before OpenVideoEx and retries 0x80a70008 for 1000 ms.
 * It is read-only from the client side and checks the service readiness word
 * returned by GetStaticInfo(21). Offset is relative to sceVshAvcapInit
 * (libSceAvcap.elf +0x940; private function at +0x18e0). */
/* libSceAvcap.elf internal ring helpers, relative to sceVshAvcapInit:
 *   0x1c10  dequeue: (state, info_out, info_size, payload_ptr_out) -> len
 *   0x1db0  consume: commits state+0x14 into state+0x10
 * The public ReadVideo at +0x14a0 does exactly dequeue -> memcpy -> consume;
 * calling the two helpers directly avoids needing a target destination buffer. */

static FILE *report;
static pbs_inject_t ctx;
static int attached;
static intptr_t fn_init, fn_open, fn_start, fn_stop, fn_close;
static intptr_t fn_info, fn_set_info, fn_static;
static intptr_t fn_idle;
static intptr_t fn_status;
static int status_probe_attempted;
/* Native RemotePlay ensure-open path: the live session object owns the
 * admission/client state that a standalone OpenVideoEx call does not. */
static intptr_t fn_native_ensure;
static intptr_t native_context;
static int native_started;

#if PBS_PS4_REMOTE_LIFECYCLE
static intptr_t fn_pre_notify, fn_set_status, fn_acquire_budget, fn_release_budget;
static int lifecycle_budget_acquired;
static int lifecycle_status_set;
static int lifecycle_uncertain;
#endif

static const char *const status_modules[] = {
  /* FW11 RemotePlay imports these symbols as #W#N: library W is
   * libSceShellCoreUtil, but provider module N is libSceSystemService.
   * An imported library name is not necessarily a loadable SPRX basename. */
  "libSceSystemService.sprx",
  "libSceSystemService",
  "libSceShellCoreUtil.sprx",
  "libSceShellCoreUtil",
  "libSceShellCoreUtil.prx",
  "SceShellCoreUtil",
  "Sce.Vsh.ShellCoreUtilWrapper.dll.sprx",
};

static void *target_ptr(intptr_t value) { return (void *)value; }

static intptr_t resolve_first(const char *symbol, const char *const *modules,
                              size_t count) {
  for (size_t i = 0; i < count; ++i) {
    intptr_t fn = pbs_resolve_in(ctx.pid, modules[i], symbol);
    if (fn) return fn;
  }
  return 0;
}

/* pbs_inject_call_fast returns LONG_MIN when the remote call did not return
 * to its trap. Truncating that to int would look like success on some paths,
 * so every wrapper maps it to a negative Avcap-style failure. */
static int remote_rc(long rc) {
  if (rc == PBS_CALL_TIMEOUT) return -1;
  return (int)rc;
}

static int remote_ready(void) { return attached && !ctx.asserted; }

static uint64_t milliseconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void log_static_probe(const char *stage, unsigned type) {
  uint32_t value = 0;
  if (!remote_ready() || !fn_static) {
    fprintf(report, "Remote Avcap static-info(%u) %s: unavailable\n", type, stage);
    return;
  }
  memset(&value, 0, sizeof value);
  if (pbs_inject_write_at(&ctx, ctx.scratch + PBS_PS4R_STATIC_OFF,
                          &value, sizeof value)) {
    fprintf(report, "Remote Avcap static-info(%u) %s: scratch write failed\n", type, stage);
    return;
  }
  long rc = pbs_inject_call_fast(&ctx, fn_static, (uint64_t[6]){
      type, (uint64_t)(ctx.scratch + PBS_PS4R_STATIC_OFF), sizeof value, 0, 0, 0});
  if (rc < 0 || pbs_inject_read_at(&ctx, ctx.scratch + PBS_PS4R_STATIC_OFF,
                                   &value, sizeof value)) {
    fprintf(report, "Remote Avcap static-info(%u) %s: rc=%08x\n",
            type, stage, (unsigned)rc);
    return;
  }
  if (type == 24)
    fprintf(report, "Remote Avcap encoder slots %s: rc=%08x mask=%08x slot0=%u (read-only)\n",
            stage, (unsigned)rc, value, value & 1u);
  else
    fprintf(report, "Remote Avcap static-info(%u) %s: rc=%08x flags=%08x native_bit5=%u (read-only)\n",
            type, stage, (unsigned)rc, value, !!(value & 0x20u));
}

static void probe_status(void) {
  if (status_probe_attempted) return;
  status_probe_attempted = 1;
  fn_status = resolve_first("sceShellCoreUtilGetRemotePlayStatus",
                            status_modules,
                            sizeof status_modules / sizeof status_modules[0]);
  if (!fn_status) {
    fprintf(report, "Remote ShellCore RemotePlay status getter unavailable\n");
    return;
  }
  fprintf(report, "Remote ShellCore RemotePlay status getter resolved=%p\n",
          target_ptr(fn_status));
}

int pbs_ps4_remote_status(unsigned char *status) {
  if (!status || !remote_ready()) return -1;
  probe_status();
  if (!fn_status) return -1;
  unsigned char value = 0xff;
  if (pbs_inject_write_at(&ctx, ctx.scratch + PBS_PS4R_STATUS_OFF, &value, 1))
    return -1;
  long rc = pbs_inject_call_fast(&ctx, fn_status, (uint64_t[6]){
      (uint64_t)(ctx.scratch + PBS_PS4R_STATUS_OFF), 0, 0, 0, 0, 0});
  if (pbs_inject_read_at(&ctx, ctx.scratch + PBS_PS4R_STATUS_OFF, &value, 1))
    return -1;
  *status = value;
  return remote_rc(rc);
}

#if PBS_PS4_REMOTE_LIFECYCLE
static int lifecycle_call(const char *name, intptr_t fn, uint64_t arg) {
  long rc = pbs_inject_call_fast(&ctx, fn, (uint64_t[6]){arg,0,0,0,0,0});
  fprintf(report, "Remote lifecycle: %s -> %08x\n", name, (unsigned)rc);
  if (rc == PBS_CALL_TIMEOUT) {
    lifecycle_uncertain = 1;
    ctx.asserted = 1; /* Quarantine subsequent remote calls, including control. */
    return -1;
  }
  return remote_rc(rc) == 0 ? 0 : -1;
}

static int lifecycle_start(void) {
  unsigned char before = 0xff, after = 0xff;
  if (lifecycle_uncertain || !fn_set_status || !fn_pre_notify ||
      !fn_acquire_budget || !fn_release_budget ||
      pbs_ps4_remote_status(&before) != 0 || before != 0) {
    fprintf(report, "Remote lifecycle refused: missing exports, uncertain state or native session active (status=%u)\n", before);
    return -1;
  }
  if (lifecycle_call("AcquireRemotePlayCpuBudget", fn_acquire_budget, 0)) return -1;
  lifecycle_budget_acquired = 1;
  if (lifecycle_call("PreNotifyOfRemotePlay", fn_pre_notify, 0)) return -1;
  if (lifecycle_call("SetRemotePlayStatus(1)", fn_set_status, 1)) return -1;
  lifecycle_status_set = 1;
  if (pbs_ps4_remote_status(&after) != 0 || after != 1) {
    lifecycle_uncertain = 1;
    fprintf(report, "Remote lifecycle state verification failed (status=%u); no forced global rollback\n", after);
    return -1;
  }
  fprintf(report, "Remote lifecycle status confirmed 0 -> 1 before OpenVideoEx\n");
  return 0;
}

static int lifecycle_stop(void) {
  if (!attached) return 0;
  if (lifecycle_uncertain || ctx.asserted) {
    fprintf(report, "Remote lifecycle cleanup refused: remote outcome uncertain\n");
    return -1;
  }
  if (lifecycle_status_set) {
    if (lifecycle_call("SetRemotePlayStatus(0)", fn_set_status, 0)) {
      lifecycle_uncertain = 1;
      return -1;
    }
    unsigned char after = 0xff;
    if (pbs_ps4_remote_status(&after) != 0 || after != 0) {
      lifecycle_uncertain = 1;
      return -1;
    }
    lifecycle_status_set = 0;
  }
  if (lifecycle_budget_acquired) {
    if (lifecycle_call("ReleaseRemotePlayCpuBudget", fn_release_budget, 0)) {
      lifecycle_uncertain = 1;
      return -1;
    }
    lifecycle_budget_acquired = 0;
  }
  return 0;
}
#endif

int pbs_ps4_remote_capture_begin(FILE *log) {
  report = log;
  return 0;
}

int pbs_ps4_remote_capture_active(void) { return attached; }

int pbs_ps4_remote_prepare_release(void) {
#if PBS_PS4_REMOTE_LIFECYCLE
  return lifecycle_stop();
#else
  return 0;
#endif
}

int pbs_ps4_remote_capture_init(void) {
  if (attached) return 0;
  native_context = 0;
  fn_native_ensure = 0;
  native_started = 0;
  if (!report) return -1;
  pbs_inject_t *shared = pbs_ps4_control_ctx();
  if (!shared) {
    /* A second PT_ATTACH after the control phase panicked the FW11 kernel in
     * the 19:08 run. Capture must use the attachment that already performed
     * Init/SetStaticInfo; if it was not retained, abort without attaching. */
    fprintf(report, "Remote Avcap no retained control context; refusing a second PT_ATTACH\n");
    return -1;
  }
  ctx = *shared;
  if (!ctx.firmware) {
    fprintf(report, "Remote Avcap missing verified firmware profile\n");
    return -1;
  }
  fprintf(report, "Remote Avcap using retained control context pid=%d scratch=%p\n",
          (int)ctx.pid, target_ptr(ctx.scratch));
  if (pbs_inject_ensure_stopped(&ctx) || pbs_inject_revalidate_scratch(&ctx)) {
    fprintf(report, "Remote Avcap retained context unsafe (stop/scratch); aborting capture\n");
    return -1;
  }
#if PBS_PS4_WORKER_BOOT
  /* Dedicated-worker boot inside the only safe attached window: the context
   * above is the retained control attachment with the gates patched. Boot
   * creates the thread, detaches and drives capture calls over loopback. */
  {
    int boot_rc = pbs_ps4_worker_boot(&ctx, report);
    *shared = ctx; /* Publish detach and private scratch even on failure. */
    if (boot_rc) {
      fprintf(report, "worker boot failed; refusing to continue without it\n");
      return -1;
    }
    /* The context is detached now and every injected call and remote memory
     * access is served by the worker thread over loopback. */
    attached = 1;
  }
#endif
  fn_init = pbs_resolve_in(ctx.pid, "libSceAvcap.sprx", "sceVshAvcapInit");
  fn_open = pbs_resolve_in(ctx.pid, "libSceAvcap.sprx", "sceVshAvcapOpenVideoEx");
  fn_start = pbs_resolve_in(ctx.pid, "libSceAvcap.sprx", "sceVshAvcapStart");
  fn_stop = pbs_resolve_in(ctx.pid, "libSceAvcap.sprx", "sceVshAvcapStop");
  fn_close = pbs_resolve_in(ctx.pid, "libSceAvcap.sprx", "sceVshAvcapClose");
  fn_info = pbs_resolve_in(ctx.pid, "libSceAvcap.sprx", "sceVshAvcapGetInfo");
  fn_set_info = pbs_resolve_in(ctx.pid, "libSceAvcap.sprx", "sceVshAvcapSetInfo");
  fn_static = pbs_resolve_in(ctx.pid, "libSceAvcap.sprx", "sceVshAvcapGetStaticInfo");
  if (!fn_init || !fn_open || !fn_start || !fn_stop || !fn_close || !fn_info) {
    fprintf(report,
            "Remote Avcap missing symbols Init=%d Open=%d Start=%d Stop=%d Close=%d Info=%d\n",
            !!fn_init, !!fn_open, !!fn_start, !!fn_stop, !!fn_close, !!fn_info);
    return -1;
  }
  attached = 1;
#if PBS_PS4_REMOTE_IDLE_RUN
  fn_idle = pbs_resolve_in(ctx.pid, "libkernel_sys.sprx", "usleep");
  if (!fn_idle) {
    fprintf(report, "Remote Avcap idle-run requires native usleep; refusing capture\n");
    pbs_ps4_remote_capture_end();
    return -1;
  }
  fprintf(report, "Remote Avcap idle-run: native usleep(10000), no SIGSTOP/reattach\n");
#endif
#if PBS_PS4_REMOTE_RING_COPY
  fprintf(report, "Remote Avcap reader: direct mapped ring, no dequeue/consume RPC\n");
#endif
#ifdef PBS_PS4
  if (ctx.trap <= 0) ctx.trap = pbs_ps4_firmware_trap(ctx.firmware, fn_init);
#endif
  fprintf(report, "Remote Avcap client pid=%d scratch=%p dequeue=%p consume=%p trap=%p kernel=%p-%p\n",
          (int)ctx.pid, target_ptr(ctx.scratch),
          target_ptr(fn_init + ctx.firmware->dequeue_delta),
          target_ptr(fn_init + ctx.firmware->consume_delta),
          target_ptr(ctx.trap), (void *)ctx.kbase, (void *)ctx.kend);
  /* Read-only native-class diagnostics. The local client and the RemotePlay
   * client are admitted by different service classes on FW11; these values
   * must be compared with the local probes in capture.txt. */
  log_static_probe("before OpenVideoEx", 21);
  log_static_probe("before OpenVideoEx", 24);
#ifdef PBS_PS4_OBSERVE_REMOTEPLAY_STATUS
  probe_status();
#endif
#if PBS_PS4_REMOTE_LIFECYCLE
  fn_pre_notify = resolve_first("sceShellCoreUtilPreNotifyOfRemotePlay",
                                status_modules,
                                sizeof status_modules / sizeof status_modules[0]);
  fn_set_status = resolve_first("sceShellCoreUtilSetRemotePlayStatus",
                                status_modules,
                                sizeof status_modules / sizeof status_modules[0]);
  fn_acquire_budget = resolve_first("sceShellCoreUtilAcquireRemotePlayCpuBudget",
                                    status_modules,
                                    sizeof status_modules / sizeof status_modules[0]);
  fn_release_budget = resolve_first("sceShellCoreUtilReleaseRemotePlayCpuBudget",
                                    status_modules,
                                    sizeof status_modules / sizeof status_modules[0]);
  fprintf(report, "Remote lifecycle symbols pre=%d set=%d acquire=%d release=%d\n",
          !!fn_pre_notify, !!fn_set_status, !!fn_acquire_budget, !!fn_release_budget);
  if (lifecycle_start()) {
    pbs_ps4_remote_capture_end();
    return -1;
  }
#endif
  if (ctx.asserted) {
    fprintf(report, "Remote Avcap client asserted during initialization\n");
    pbs_ps4_remote_capture_end();
    return -1;
  }
  return 0;
}

int pbs_ps4_remote_capture_end(void) {
  if (!attached) return 0;
  int failed = 0;
#if PBS_PS4_LATCH_BYPASS || PBS_PS4_NATIVE_SOURCE
  if (pbs_ps4_latch_patch_restore(report)) failed = -1;
#endif
#if PBS_PS4_BUDGET_UNLOCK
  if (pbs_ps4_latch_budget_set(report, 0)) failed = -1;
#endif
#if PBS_PS4_REMOTE_LIFECYCLE
  if (lifecycle_stop()) failed = -1;
#endif
  pbs_inject_t *shared = pbs_ps4_control_ctx();
  if (shared) {
    /* Keep the connection for the control phase's SetStaticInfo(21,0)/Term.
     * It will detach after the final remote operation. */
    *shared = ctx;
  }
#if PBS_PS4_LOW_MEMORY
  /* The bounded experiment retains its cleanup owner to retry a failed
   * latch restore after Stop/Close, before releasing the control context. */
  if (failed) return -1;
#endif
  attached = 0;
  fn_init = fn_open = fn_start = fn_stop = fn_close = 0;
  fn_info = fn_set_info = fn_static = 0;
  fn_idle = 0;
  fn_status = 0;
  status_probe_attempted = 0;
  native_context = 0;
  fn_native_ensure = 0;
  native_started = 0;
  return failed;
}

/* Native ordering: a bounded readiness negotiation runs before OpenVideoEx.
 * It is safe (GetStaticInfo only) and returns 0x80a70008 while the service
 * class is not ready. PBS never writes the readiness word. */
static void wait_native_ready(unsigned profile) {
  const intptr_t ready = fn_init ? fn_init + ctx.firmware->ready_delta : 0;
  if (!remote_ready() || !ready) return;
  unsigned char desc[0x10];
  memset(desc, 0, sizeof desc);
  int full_hd = profile == 2 || profile == 20 || profile == 24;
  uint16_t width = full_hd ? 1920 : 1280;
  uint16_t height = full_hd ? 1080 : 720;
  memcpy(desc + 4, &width, sizeof width);
  memcpy(desc + 6, &height, sizeof height);
  memcpy(desc + 8, &width, sizeof width);
  memcpy(desc + 10, &height, sizeof height);
  uint32_t pixel_format = 0x20;
  memcpy(desc + 12, &pixel_format, sizeof pixel_format);
  if (pbs_inject_write_at(&ctx, ctx.scratch + PBS_PS4R_DESC_OFF, desc, sizeof desc)) {
    fprintf(report, "Remote Avcap readiness descriptor write failed\n");
    return;
  }
  const uint64_t began = milliseconds();
  unsigned tries = 0;
  long rc = 0;
  do {
    rc = pbs_inject_call_fast(&ctx, ready, (uint64_t[6]){
        (uint64_t)(ctx.scratch + PBS_PS4R_DESC_OFF), 1, 0, 0, 0, 0});
    if (rc != 0x80a70008) break;
    ++tries;
    usleep(16000);
  } while (milliseconds() - began < 1000);
  fprintf(report, "Remote Avcap readiness profile=%u -> %08x retries=%u elapsed_ms=%llu\n",
          profile, (unsigned)rc, tries,
          (unsigned long long)(milliseconds() - began));
}

/* PS4 SDK module helper. Declared here instead of including <ps4/kernel.h>
 * so the host regression can provide its own stub. */
extern int kernel_dynlib_handle(pid_t pid, const char *basename,
                                uint32_t *handle);
extern intptr_t kernel_dynlib_mapbase_addr(pid_t pid, uint32_t handle);

static intptr_t remoteplay_base(void) {
  /* Resolve the main module only through named export lookup, the same API
   * already proven for libSceAvcap. kernel_dynlib_obj is deliberately unused:
   * its firmware struct layout differs from the SDK header and reading it
   * preceded two console kernel panics (20:59 and 21:48 on 17/10/2026). */
  static const unsigned char ensure_open_sig[32] = {
      0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
      0x53, 0x48, 0x83, 0xec, 0x28, 0x4c, 0x8b, 0x35, 0x48, 0x56, 0x23, 0x00,
      0x49, 0x89, 0xfd, 0x48, 0x8d, 0x77, 0x70, 0x48};
  static const char *modules[] = {
      "eboot.bin", "remoteplay.elf", "SceRemotePlay", "SceRemotePlay.elf",
      "remoteplay"};
  for (size_t m = 0; m < sizeof modules / sizeof modules[0]; ++m) {
    uint32_t handle = 0;
    if (kernel_dynlib_handle(ctx.pid, modules[m], &handle)) continue;
    intptr_t base = kernel_dynlib_mapbase_addr(ctx.pid, handle);
    if (!base) continue;
    unsigned char sig[sizeof ensure_open_sig];
    if (!pbs_inject_read_at(&ctx, base + (intptr_t)ctx.firmware->rp_ensure_open,
                            sig, sizeof sig) &&
        !memcmp(sig, ensure_open_sig, sizeof sig)) {
      fprintf(report, "Native RemotePlay base via module=%s handle=%u base=%p\n",
              modules[m], handle, target_ptr(base));
      return base;
    }
    fprintf(report, "Native RemotePlay module=%s base=%p rejected (no ensure-open signature)\n",
            modules[m], target_ptr(base));
  }
  static const char *symbols[] = {"nn_socket", "nJ3r2-TtBSU#v#A"};
  for (size_t m = 0; m < sizeof modules / sizeof modules[0]; ++m) {
    for (size_t s = 0; s < sizeof symbols / sizeof symbols[0]; ++s) {
      intptr_t fn = pbs_resolve_in(ctx.pid, modules[m], symbols[s]);
      if (!fn) continue;
      fprintf(report, "Native RemotePlay base via module=%s symbol=%s fn=%p\n",
              modules[m], symbols[s], target_ptr(fn));
      return fn - (intptr_t)ctx.firmware->rp_nn_socket;
    }
  }
  fprintf(report, "Native RemotePlay module base unavailable\n");
  return 0;
}

/* Find the native session wrapper whose vtable is base+0x529838. Its +0x10
 * field is the live session object accepted by the module's ensure-open
 * function. The wrapper is normally a module global, but the stack fallback
 * covers a callback context built by the native event loop. */
static intptr_t scan_native_wrapper(intptr_t base, intptr_t start, size_t size,
                                    const char *label) {
  const uint64_t vtable = (uint64_t)(base + (intptr_t)ctx.firmware->rp_vtable);
  unsigned char buf[0x1000];
  for (size_t off = 0; off < size; off += sizeof buf) {
    size_t chunk = size - off;
    if (chunk > sizeof buf) chunk = sizeof buf;
    if (pbs_inject_read_at(&ctx, start + (intptr_t)off, buf, chunk)) continue;
    for (size_t i = 0; i + sizeof(uint64_t) <= chunk; i += sizeof(uint64_t)) {
      uint64_t found;
      memcpy(&found, buf + i, sizeof found);
      if (found != vtable) continue;
      intptr_t wrapper = start + (intptr_t)off + (intptr_t)i;
      intptr_t inner = 0;
      if (pbs_inject_read_at(&ctx, wrapper + 0x10, &inner, sizeof inner) ||
          !inner)
        continue;
      uint32_t fps = 0;
      uint64_t handle = 0;
      unsigned char opened = 0xff;
      if (pbs_inject_read_at(&ctx, inner + 0x1a0, &fps, sizeof fps) ||
          pbs_inject_read_at(&ctx, inner + 0x68, &handle, sizeof handle) ||
          pbs_inject_read_at(&ctx, inner + 0x3c8, &opened, sizeof opened) ||
          opened > 1)
        continue;
      fprintf(report,
              "Native RemotePlay context %s wrapper=%p inner=%p fps=%u handle=%p opened=%u\n",
              label, target_ptr(wrapper), target_ptr(inner), fps,
              target_ptr((intptr_t)handle), opened);
      return inner;
    }
  }
  return 0;
}

static intptr_t find_native_context(void) {
  if (!PBS_PS4_NATIVE_ENSURE_OPEN) return 0;
  if (native_context) return native_context;
  intptr_t base = remoteplay_base();
  if (!base) return 0;
  fprintf(report, "Native RemotePlay base=%p vtable=%p data=%p\n",
          target_ptr(base), target_ptr(base + (intptr_t)ctx.firmware->rp_vtable),
          target_ptr(base + (intptr_t)ctx.firmware->rp_data));
  intptr_t inner = scan_native_wrapper(
      base, base + (intptr_t)ctx.firmware->rp_data, ctx.firmware->rp_data_size, "module-data");
  if (!inner) {
    intptr_t rsp = 0;
    if (!pbs_inject_target_rsp(&ctx, &rsp) && rsp > 0x20000) {
      inner = scan_native_wrapper(base, rsp - 0x10000, 0x20000, "stack");
    }
  }
  if (!inner) {
    fprintf(report, "Native RemotePlay context not found in module data or stack\n");
    return (intptr_t)-1;
  }
  native_context = inner;
  fn_native_ensure = base + (intptr_t)ctx.firmware->rp_ensure_open;
  fprintf(report, "Native RemotePlay ensure-open=%p this=%p\n",
          target_ptr(fn_native_ensure), target_ptr(native_context));
  return native_context;
}

/* 1 = native open/start succeeded, 0 = context unavailable (use the direct
 * Avcap path), -1 = live context found but native open/start failed. */
static int try_native_open(void **handle) {
  intptr_t inner = find_native_context();
  if (inner == (intptr_t)-1) return -1;
  if (!inner) return 0;
  uint64_t before_handle = 0, before_open = 0;
  unsigned char before_opened = 0;
  pbs_inject_read_at(&ctx, inner + 0x68, &before_handle, sizeof before_handle);
  pbs_inject_read_at(&ctx, inner + 0x90, &before_open, sizeof before_open);
  pbs_inject_read_at(&ctx, inner + 0x3c8, &before_opened, 1);
  fprintf(report,
          "Native RemotePlay ensure-open before handle=%p open_out=%p opened=%u\n",
          target_ptr((intptr_t)before_handle), target_ptr((intptr_t)before_open),
          before_opened);
  long rc = pbs_inject_call_fast(&ctx, fn_native_ensure,
                                 (uint64_t[6]){(uint64_t)inner, 0, 0, 0, 0, 0});
  uint64_t after_handle = 0, after_open = 0;
  unsigned char after_opened = 0;
  pbs_inject_read_at(&ctx, inner + 0x68, &after_handle, sizeof after_handle);
  pbs_inject_read_at(&ctx, inner + 0x90, &after_open, sizeof after_open);
  pbs_inject_read_at(&ctx, inner + 0x3c8, &after_opened, 1);
  fprintf(report,
          "Native RemotePlay ensure-open rc=%016llx ok=%d handle=%p open_out=%p opened=%u\n",
          (unsigned long long)rc, (int)(rc != PBS_CALL_TIMEOUT && (rc & 0xff) == 1),
          target_ptr((intptr_t)after_handle), target_ptr((intptr_t)after_open),
          after_opened);
  if (rc == PBS_CALL_TIMEOUT) return -1;
  if ((rc & 0xff) != 1) {
    /* A clean decline that left no handle and no "opened" flag is safe to
     * follow with the proven direct OpenVideoEx path; a partial state is not,
     * because a second open could double-claim the encoder slot. */
    if (after_opened || after_handle || after_open) {
      fprintf(report, "Native RemotePlay ensure-open partial state; refusing direct Open\n");
      return -1;
    }
    fprintf(report, "Native RemotePlay ensure-open declined; using direct OpenVideoEx\n");
    return 0;
  }
  uint64_t native_handle = after_handle ? after_handle : after_open;
  if (!native_handle) {
    fprintf(report, "Native RemotePlay ensure-open produced no handle\n");
    return -1;
  }
  *handle = target_ptr((intptr_t)native_handle);
  native_started = 1;
  return 1;
}

int pbs_ps4_remote_open(void **handle, const void *params) {
  if (!remote_ready() || !handle || !params) return -1;
  int native = try_native_open(handle);
  if (native > 0) {
    fprintf(report, "Remote Avcap native ensure-open accepted handle=%p\n", *handle);
    return 0;
  }
  if (native < 0) {
    fprintf(report, "Remote Avcap native ensure-open failed; not retrying direct OpenVideoEx\n");
    return -1;
  }
  const uint32_t *request = params;
  const unsigned profile = request[0x08 / 4];
#if PBS_PS4_ADMISSION_PROBE
  if (!ctx.worker || pbs_ps4_admission_snapshot(report, ctx.firmware,
                                               "before OpenVideoEx", profile)) {
    fprintf(report, "Admission diagnostic unavailable; refusing experimental Open\n");
    return -1;
  }
#endif
#if PBS_PS4_REMOTE_1080
  if (profile == 24 && request[3] == 2 &&
      pbs_ps4_admission_snapshot(report, ctx.firmware,
                                 "before RemotePlay 24/2 Open", profile)) {
    fprintf(report, "RemotePlay 24/2 read-only diagnostic unavailable; refusing Open\n");
    return -1;
  }
#endif
  wait_native_ready(profile);
  if (pbs_inject_write_at(&ctx, ctx.scratch + PBS_PS4R_REQUEST_OFF,
                          request, PBS_PS4_VIDEO_REQUEST_SIZE)) {
    fprintf(report, "Remote Avcap request write failed\n");
    return -1;
  }
#if PBS_PS4_LATCH_BYPASS || PBS_PS4_NATIVE_SOURCE
  if (profile != (PBS_PS4_NATIVE_SOURCE ? 24u : 2u) ||
      request[3] != (PBS_PS4_NATIVE_SOURCE ? 24u : 2u) || request[1] != 0 ||
      request[0x1c / 4] != 30 || request[0x54 / 4] != 30 ||
      pbs_ps4_latch_patch_apply(report, ctx.firmware)) {
    fprintf(report, "Experimental latch patch unavailable; refusing Open\n");
    pbs_ps4_latch_patch_restore(report);
    return -1;
  }
#endif
#if PBS_PS4_BUDGET_UNLOCK
  if (profile != 2 || request[3] != 2 || request[1] != 0 ||
      request[0x1c / 4] != 30 || request[0x54 / 4] != 30 ||
      pbs_ps4_latch_budget_set(report, 1)) {
    fprintf(report, "Experimental budget unlock failed; refusing Open\n");
    pbs_ps4_latch_budget_set(report, 0);
#if PBS_PS4_LATCH_BYPASS || PBS_PS4_NATIVE_SOURCE
    pbs_ps4_latch_patch_restore(report);
#endif
    return -1;
  }
  /* Armed bit 0x200 raises the decoder core budget to 54 MiB; it stays
   * armed through Open/Start and is cleared in capture_end. */
#if PBS_PS4_ADMISSION_PROBE
  pbs_ps4_admission_snapshot(report, ctx.firmware, "after budget unlock", profile);
#endif
#endif
  const uint64_t began = milliseconds();
  const uint64_t sentinel = 0xa5a5a5a5a5a5a5a5ULL;
  unsigned retries = 0, spurious = 0, attempts = 0;
  uint64_t target_handle = 0;
  long rc = 0;
  int last_handle_read_rc = 0;
  int ensure_failed = 0;
  int last_untouched = 0;
  do {
    ++attempts;
    /* The wrapper writes the client object pointer here. A value that still
     * equals the sentinel means the call returned without executing that
     * store: a false early return, not a service result. */
    target_handle = sentinel;
    if (pbs_inject_write_at(&ctx, ctx.scratch + PBS_PS4R_HANDLE_OFF,
                            &target_handle, sizeof target_handle)) {
      fprintf(report, "Remote Avcap handle scratch write failed\n");
#if PBS_PS4_BUDGET_UNLOCK
      pbs_ps4_latch_budget_set(report, 0);
#endif
#if PBS_PS4_LATCH_BYPASS || PBS_PS4_NATIVE_SOURCE
      pbs_ps4_latch_patch_restore(report);
#endif
      return -1;
    }
    const uint64_t attempt_began = milliseconds();
    rc = pbs_inject_call_fast(&ctx, fn_open, (uint64_t[6]){
        (uint64_t)(ctx.scratch + PBS_PS4R_HANDLE_OFF),
        (uint64_t)(ctx.scratch + PBS_PS4R_REQUEST_OFF), 0, 0, 0, 0});
    const uint64_t attempt_ms = milliseconds() - attempt_began;
    if (pbs_inject_ensure_stopped(&ctx)) ensure_failed = 1;
    last_handle_read_rc = pbs_inject_read_at(&ctx, ctx.scratch + PBS_PS4R_HANDLE_OFF,
                                             &target_handle, sizeof target_handle);
    if (last_handle_read_rc) target_handle = 0;
    int slot_untouched = (!last_handle_read_rc && target_handle == sentinel);
    if (slot_untouched) target_handle = 0;
    /* Only a zero return with an untouched slot is contradictory: the wrapper
     * writes the handle on its single success path. A 0x80a70008 retry leaves
     * the slot untouched as well and stays the normal transient case. */
    last_untouched = (rc == 0 && slot_untouched);
    if (attempts <= 4 || slot_untouched)
      fprintf(report,
              "Remote Avcap OpenVideoEx attempt=%u rc=%016llx handle=%016llx untouched=%d elapsed_ms=%llu\n",
              attempts, (unsigned long long)rc, (unsigned long long)target_handle,
              slot_untouched, (unsigned long long)attempt_ms);
    if (last_untouched && milliseconds() - began < 2016u) {
      ++spurious;
      usleep(16000);
      continue;
    }
    if ((uint32_t)rc != 0x80a70008u || target_handle ||
        milliseconds() - began >= 2016u) break;
    ++retries;
    usleep(16000);
  } while (milliseconds() - began < 2016u);
  if (retries)
    fprintf(report, "Remote Avcap OpenVideoEx transient 80a70008 retries=%u elapsed_ms=%llu\n",
            retries, (unsigned long long)(milliseconds() - began));
  if (spurious)
    fprintf(report, "Remote Avcap OpenVideoEx false early returns=%u (trap race)\n", spurious);
#if PBS_PS4_BUDGET_UNLOCK
  /* Open failed or produced no handle: release the armed budget bit right
   * here; on a successful Open it stays armed until capture_end. */
  if (!target_handle && pbs_ps4_latch_budget_set(report, 0))
    fprintf(report, "Budget restore failed after failed Open; armed bit stays pending\n");
#endif
#if PBS_PS4_LATCH_BYPASS || PBS_PS4_NATIVE_SOURCE
  /* FW11's producer admission reuses the same capability getter at Start.
   * Only the bounded low-memory smoke retains ownership until capture_end,
   * after Stop/Close. Failed Open always restores immediately. */
  if (!PBS_PS4_LOW_MEMORY || !target_handle || remote_rc(rc) < 0) {
    if (pbs_ps4_latch_patch_restore(report))
      fprintf(report, "Experimental patch restore failed; Start will refuse while pending\n");
  } else {
    fprintf(report, "Low-memory smoke: latch retained through Stop/Close; restored at capture_end\n");
  }
#endif
  fprintf(report,
          "Remote Avcap Open result rc=%016llx timeout=%d handle_read_rc=%d raw_handle=%016llx untouched=%d asserted=%d ensure_stopped_failed=%d\n",
          (unsigned long long)rc, rc == PBS_CALL_TIMEOUT, last_handle_read_rc,
          (unsigned long long)target_handle, last_untouched, ctx.asserted, ensure_failed);
  /* Read-only service probes: if the slot became occupied without a client
   * handle, the service accepted an open whose client-side store was lost;
   * if it is still free, the "success" never reached the encoder. */
  log_static_probe("after OpenVideoEx", 24);
  log_static_probe("after OpenVideoEx", 21);
#if PBS_PS4_ADMISSION_PROBE
  /* No second service attachment while a successful capture is running. */
  if ((uint32_t)rc && !target_handle)
    pbs_ps4_admission_snapshot(report, ctx.firmware, "after failed OpenVideoEx", profile);
#endif
  *handle = target_ptr((intptr_t)target_handle);
  return remote_rc(rc);
}

int pbs_ps4_remote_info(void *handle, unsigned type, void *out, size_t size) {
  if (!remote_ready() || !handle || !out || size > PBS_PS4R_INFO_SIZE) return -1;
  unsigned char buffer[PBS_PS4R_INFO_SIZE];
  memset(buffer, 0, sizeof buffer);
  if (pbs_inject_write_at(&ctx, ctx.scratch + PBS_PS4R_INFO_OFF, buffer, size))
    return -1;
  long rc = pbs_inject_call_fast(&ctx, fn_info, (uint64_t[6]){
      (uint64_t)handle, type, (uint64_t)(ctx.scratch + PBS_PS4R_INFO_OFF),
      size, 0, 0});
  int result = remote_rc(rc);
  if (result < 0) return result;
  /* SPS/PPS return their byte count. Fixed-size GetInfo selectors instead
   * return zero after writing the caller's buffer (FW11 +0x9281). */
  size_t copied = result ? (size_t)result : size;
  if (copied > size ||
      pbs_inject_read_at(&ctx, ctx.scratch + PBS_PS4R_INFO_OFF, out, copied))
    return -1;
  return result;
}

int pbs_ps4_remote_static_info(unsigned type, void *out, size_t size) {
  if (!remote_ready() || !fn_static || !out || size > PBS_PS4R_INFO_SIZE) return -1;
  unsigned char buffer[PBS_PS4R_INFO_SIZE];
  memset(buffer, 0, sizeof buffer);
  if (pbs_inject_write_at(&ctx, ctx.scratch + PBS_PS4R_STATIC_OFF, buffer, size))
    return -1;
  long rc = pbs_inject_call_fast(&ctx, fn_static, (uint64_t[6]){
      type, (uint64_t)(ctx.scratch + PBS_PS4R_STATIC_OFF), size, 0, 0, 0});
  if (rc >= 0 && (size_t)rc <= size) {
    if (pbs_inject_read_at(&ctx, ctx.scratch + PBS_PS4R_STATIC_OFF, out, (size_t)rc))
      return -1;
  }
  return remote_rc(rc);
}

int pbs_ps4_remote_start(void *handle) {
#if PBS_PS4_LATCH_BYPASS && !PBS_PS4_LOW_MEMORY
  if (pbs_ps4_latch_patch_pending() && pbs_ps4_latch_patch_restore(report)) return -1;
#endif
  if (!remote_ready() || !handle) return -1;
  if (native_started) {
    fprintf(report, "Remote Avcap native Start already performed\n");
    return 0;
  }
  return remote_rc(pbs_inject_call_fast(&ctx, fn_start,
                                        (uint64_t[6]){(uint64_t)handle, 0, 0, 0, 0, 0}));
}

int pbs_ps4_remote_stop(void *handle) {
  if (!remote_ready() || !handle) return -1;
  return remote_rc(pbs_inject_call_fast(&ctx, fn_stop,
                                        (uint64_t[6]){(uint64_t)handle, 0, 0, 0, 0, 0}));
}

int pbs_ps4_remote_close(void *handle) {
  if (!remote_ready() || !handle) return -1;
  return remote_rc(pbs_inject_call_fast(&ctx, fn_close,
                                        (uint64_t[6]){(uint64_t)handle, 0, 0, 0, 0, 0}));
}

/* The public ReadVideo performs: internal dequeue -> memcpy(payload) ->
 * consume. Calling the two internal helpers directly gives us the mapped
 * payload pointer, so the payload process can copy it with PT_IO without
 * requiring a destination buffer in the stopped target process. The flags
 * argument is retained for the caller's ABI; PBS only uses the nonblocking
 * ring probe contract. */
#if !PBS_PS4_REMOTE_RING_COPY
int pbs_ps4_remote_read(void *handle, void *out, size_t size, void *info, int flags) {
  (void)flags;
  int result = -1;
  if (!remote_ready() || !handle || !out || !fn_init) return -1;
  if (pbs_inject_ensure_stopped(&ctx) || pbs_inject_revalidate_scratch(&ctx)) {
    fprintf(report, "Remote Avcap read context unsafe (stop/scratch)\n");
    return -1;
  }
  const intptr_t state = (intptr_t)handle + ctx.firmware->ring_state_offset;
  const intptr_t dequeue = fn_init + ctx.firmware->dequeue_delta;
  const intptr_t consume = fn_init + ctx.firmware->consume_delta;
  uint64_t payload = 0;
  unsigned char zero_info[PBS_PS4R_META_SIZE] = {0};
  int nudges_before = ctx.nudges;
  if (pbs_inject_write_at(&ctx, ctx.scratch + PBS_PS4R_PACKET_OFF,
                          &payload, sizeof payload) ||
      pbs_inject_write_at(&ctx, ctx.scratch + PBS_PS4R_INFO_OFF,
                          zero_info, sizeof zero_info)) {
    fprintf(report, "Remote Avcap read scratch write failed (stop/scratch)\n");
    goto done;
  }
  long length = pbs_inject_call_fast(&ctx, dequeue, (uint64_t[6]){
      (uint64_t)state, (uint64_t)(ctx.scratch + PBS_PS4R_INFO_OFF),
      PBS_PS4R_META_SIZE, (uint64_t)(ctx.scratch + PBS_PS4R_PACKET_OFF), 0, 0});
  if (ctx.nudges != nudges_before)
    fprintf(report, "Remote Avcap dequeue needed SIGCONT nudge (total=%d)\n", ctx.nudges);
  if (length == PBS_CALL_TIMEOUT) {
    fprintf(report, "Remote Avcap dequeue call timed out (trap not reached) lwp=%d rip=%p kernel=%d\n",
            (int)ctx.lwp, target_ptr(ctx.lwp_rip), ctx.lwp_kernel);
    result = -1;
    goto done;
  }
  /* The internal dequeue reports its own status codes in RAX, and they are
   * positive when widened to 64 bits. 0x80A70002 means "ring empty" and must
   * never be mistaken for a 2 GiB payload (hardware run 20:54:28 discarded
   * every other frame with "length 2158428162 exceeds local buffer"). */
  int packet_length = remote_rc(length);
  if ((uint32_t)packet_length == 0x80a70002u ||
      (uint32_t)packet_length == 0x80a70004u) {
    result = 0;
    goto done;
  }
  if (packet_length <= 0) {
    result = packet_length;
    goto done;
  }
  if ((size_t)length > size) {
    fprintf(report, "Remote Avcap packet length %ld exceeds local buffer %zu\n",
            length, size);
    pbs_inject_call_fast(&ctx, consume, (uint64_t[6]){(uint64_t)state, 0, 0, 0, 0, 0});
    goto done;
  }
  if (pbs_inject_read_at(&ctx, ctx.scratch + PBS_PS4R_PACKET_OFF,
                         &payload, sizeof payload) || !payload) {
    fprintf(report, "Remote Avcap packet pointer unavailable length=%ld\n", length);
    pbs_inject_call_fast(&ctx, consume, (uint64_t[6]){(uint64_t)state, 0, 0, 0, 0, 0});
    goto done;
  }
  if (pbs_inject_read_at(&ctx, (intptr_t)payload, out, (size_t)length)) {
    fprintf(report, "Remote Avcap packet PT_IO read failed length=%ld payload=%p\n",
            length, target_ptr((intptr_t)payload));
    pbs_inject_call_fast(&ctx, consume, (uint64_t[6]){(uint64_t)state, 0, 0, 0, 0, 0});
    goto done;
  }
  int info_failed = info &&
      pbs_inject_read_at(&ctx, ctx.scratch + PBS_PS4R_INFO_OFF, info,
                         PBS_PS4R_META_SIZE);
  long consume_rc = pbs_inject_call_fast(&ctx, consume,
                                         (uint64_t[6]){(uint64_t)state, 0, 0, 0, 0, 0});
  if (remote_rc(consume_rc) < 0) {
    fprintf(report, "Remote Avcap ring consume failed rc=%016llx length=%ld\n",
            (unsigned long long)consume_rc, length);
    result = remote_rc(consume_rc);
    goto done;
  }
  /* Do not publish stale metadata or a frame whose ring entry might be read
   * again after a failed consume. The public ReadVideo propagates consume
   * failures too (libSceAvcap +0x1591..+0x1598). */
  result = info_failed ? -1 : packet_length;
done:
  /* Keep the target stopped for the whole capture. Every resume attempt made
   * the following SIGSTOP park a syscall thread, and the kernel then rewound
   * the RIP we installed, so the next dequeue never returned (12 s timeouts
   * in three hardware runs). Whether the encoder still feeds the ring while
   * SceRemotePlay is frozen is exactly what this run measures: reads that
   * return 0x80a70002 prove an empty ring, not a broken call path. */
  return result;
}
#else
/* Copy the mapped ring through the selected transport. With the worker,
 * SceRemotePlay runs normally; capture owns its handle and cursor. The service
 * is the producer. Copying does not borrow a native thread or its stack.
 * The cursor writes reproduce libSceAvcap +0x1db0 exactly; all other accesses
 * are reads. Recheck producer overrun after the copy before publishing it. */
static int ring_commit(intptr_t state, int32_t read, uint32_t generation) {
  uint32_t cursor[3] = {(uint32_t)read, (uint32_t)read, generation};
  return pbs_inject_write_at(&ctx, state + 0x10, cursor, sizeof cursor);
}

int pbs_ps4_remote_read(void *handle, void *out, size_t size, void *info, int flags) {
  (void)flags;
  if (!remote_ready() || !handle || !out) return -1;
  const intptr_t state = (intptr_t)handle + ctx.firmware->ring_state_offset;
  pbs_ps4_ring_cursor c = {0};
  pbs_ps4_ring_header h, after;
  pbs_ps4_ring_entry e;
  if (pbs_inject_read_at(&ctx, state, &c, sizeof c) || !c.mapping ||
      c.mapping > INT64_MAX - sizeof h ||
      pbs_inject_read_at(&ctx, (intptr_t)c.mapping, &h, sizeof h)) return -1;
  if (h.size < 0) return h.size;
  if (!pbs_ps4_ring_valid(&c, &h)) {
    fprintf(report, "Remote Avcap invalid ring map=%016llx mapped=%llu size=%d read=%d write=%d boundary=%d generations=%u/%u\n",
            (unsigned long long)c.mapping, (unsigned long long)c.mapping_size,
            h.size, c.read, h.write, h.oldest, c.generation, h.generation);
    return -1;
  }
  if (c.read == h.write && c.generation == h.generation) {
#if PBS_PS4_REMOTE_IDLE_RUN
    long rc = pbs_inject_call_fast(&ctx, fn_idle,
                                  (uint64_t[6]){10000,0,0,0,0,0});
    if (rc == PBS_CALL_TIMEOUT || remote_rc(rc) < 0) {
      fprintf(report, "Remote Avcap idle-run failed; ending capture\n");
      return -1;
    }
#endif
    return 0;
  }
  if (pbs_ps4_ring_lapped(&h, c.read, c.generation)) {
    if (ring_commit(state, h.write, h.generation)) return -1;
    return (int32_t)0x80a70004u;
  }
  int32_t entry_at = h.size - c.read < 16 ? 16 : c.read;
  if (pbs_inject_read_at(&ctx, (intptr_t)c.mapping + entry_at, &e, sizeof e))
    return -1;
  if (e.metadata_size < 0 || e.metadata_size > h.size - 16 ||
      e.reservation < 0 || e.reservation > h.size - 16 ||
      e.length < 0 || e.length > e.reservation) {
    fprintf(report, "Remote Avcap invalid ring entry at=%d metadata=%d reservation=%d length=%d\n",
            entry_at, e.metadata_size, e.reservation, e.length);
    return -1;
  }
  int32_t metadata_at = entry_at + 16;
  if (h.size - metadata_at < e.metadata_size) metadata_at = 16;
  int32_t payload_at = metadata_at + e.metadata_size;
  if (h.size - payload_at < e.reservation) payload_at = 16;
  int64_t next = (int64_t)payload_at + (((int64_t)e.length + 3) & ~INT64_C(3));
  if (next > h.size) return -1;
  unsigned char metadata[PBS_PS4R_META_SIZE] = {0};
  size_t meta_size = (size_t)e.metadata_size;
  if (meta_size > sizeof metadata) meta_size = sizeof metadata;
  int too_large = (size_t)e.length > size;
  if ((info && meta_size && pbs_inject_read_at(&ctx,
          (intptr_t)c.mapping + metadata_at, metadata, meta_size)) ||
      (!too_large && e.length && pbs_inject_read_at(&ctx,
          (intptr_t)c.mapping + payload_at, out, (size_t)e.length)) ||
      pbs_inject_read_at(&ctx, (intptr_t)c.mapping, &after, sizeof after))
    return -1;
  if (after.size < 0) return after.size;
  if (!pbs_ps4_ring_valid(&c, &after) || after.size != h.size) return -1;
  if (pbs_ps4_ring_lapped(&after, c.read, c.generation)) {
    if (ring_commit(state, after.write, after.generation)) return -1;
    return (int32_t)0x80a70004u;
  }
  uint32_t generation = c.generation + (c.read > next);
  if (ring_commit(state, (int32_t)next, generation)) return -1;
  if (too_large) return -1;
  if (info) memcpy(info, metadata, sizeof metadata);
  return e.length;
}
#endif

int pbs_ps4_remote_keyframe(void *handle) {
  if (!remote_ready() || !handle || !fn_set_info) return -1;
  return remote_rc(pbs_inject_call_fast(&ctx, fn_set_info,
                                        (uint64_t[6]){(uint64_t)handle, 3, 0, 0, 0, 0}));
}
