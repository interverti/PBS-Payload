// SPDX-License-Identifier: GPL-3.0-or-later
/* Host-only test for the SceRemotePlay-injected PS4 Avcap backend.
 * It emulates the ptrace call/scratch ABI and verifies the exact request,
 * readiness retry, header retrieval, ring dequeue/copy/consume and balanced
 * cleanup paths. No console, no network and no real Avcap service are used. */
#include "common/process/inject.h"
#include "platform/ps4/capture/remote.h"
#include "platform/ps4/capture/request.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if PBS_PS4_ADMISSION_PROBE || PBS_PS4_REMOTE_1080
static int snapshot_fail, snapshot_calls;
static uint32_t forced_open_error;
int pbs_ps4_admission_snapshot(FILE *log, const pbs_ps4_firmware *fw,
                                const char *stage, unsigned profile) {
  assert(log && fw && stage && (profile == 2 || profile == 19 || profile == 20 || profile == 24));
  ++snapshot_calls;
  return snapshot_fail ? -1 : 0;
}
#endif

#define TARGET_SIZE 0x800000u
#define SCRATCH_BASE 0x100000u
#define RP_BASE 0x100000u
#define RP_NN_SOCKET (RP_BASE + 0x1FDF60u)
#define RP_VTABLE (RP_BASE + 0x529838u)
#define RP_DATA (RP_BASE + 0x538000u)
#define NATIVE_INNER 0x300000u
#define NATIVE_ENSURE_FN (RP_BASE + 0x2FD250u)
#define INIT_FN 0x4000u
#define READY_FN (INIT_FN + 0xFA0u)
#define DEQUEUE_FN (INIT_FN + 0x12D0u)
#define CONSUME_FN (INIT_FN + 0x1470u)
#define OPEN_FN 0x4100u
#define START_FN 0x4200u
#define STOP_FN 0x4300u
#define CLOSE_FN 0x4400u
#define INFO_FN 0x4500u
#define SETINFO_FN 0x4600u
#define STATIC_FN 0x4700u
#define IDLE_FN 0x4800u
#define STATUS_FN 0x4900u
#define ACQUIRE_FN 0x4a00u
#define PRENOTIFY_FN 0x4b00u
#define SETSTATUS_FN 0x4c00u
#define RELEASE_FN 0x4d00u
#define FRAME_ADDR 0x200000u
#define FRAME_LEN 100u

static unsigned char target[TARGET_SIZE];
static int ready_calls, open_calls, start_calls, stop_calls, close_calls;
static int info_calls, static_calls, dequeue_calls, consume_calls;
static int idle_calls, fail_idle;
static unsigned char native_status = 1;
static int lifecycle_missing, lifecycle_fail_pre, lifecycle_budget;
static int lifecycle_acquires, lifecycle_releases, lifecycle_sets;
static int lifecycle_fail_clear;
static uint64_t request_word_usage, request_word_profile;
static int fail_start;
static long dequeue_result = FRAME_LEN, consume_result;
static int fail_metadata;
static intptr_t mutate_after_read_address, mutate_header_address;
static int native_module_enabled = 1;
static intptr_t seen_trap;
#if PBS_PS4_EXPORTED_READ
static int exported_reads, exported_frees;
#endif

static void setup_native_module(void) {
  uint64_t vtable = RP_VTABLE;
  intptr_t wrapper = RP_DATA + 0x100;
  intptr_t inner = NATIVE_INNER;
  uint32_t fps = 60;
  uint64_t handle = 0;
  memcpy(target + wrapper, &vtable, sizeof vtable);
  memcpy(target + wrapper + 0x10, &inner, sizeof inner);
  memcpy(target + inner + 0x1a0, &fps, sizeof fps);
  memcpy(target + inner + 0x68, &handle, sizeof handle);
  target[inner + 0x3c8] = 0;
}

static void reset_counters(void) {
  ready_calls = open_calls = start_calls = stop_calls = close_calls = 0;
  info_calls = static_calls = dequeue_calls = consume_calls = 0;
  request_word_usage = request_word_profile = 0;
  fail_start = 0;
}

/* The control phase retains a stopped, page-validated context; this fake
 * context lets the test exercise the same single-attach path without ptrace. */
static pbs_inject_t control_ctx_stub;
pbs_inject_t *pbs_ps4_control_ctx(void) {
  control_ctx_stub.firmware = pbs_ps4_firmware_find(0x11008001u);
#if PBS_PS4_EXPORTED_READ
  static pbs_ps4_firmware opaque_profile;
  opaque_profile = *control_ctx_stub.firmware;
  opaque_profile.ready_delta = opaque_profile.dequeue_delta =
      opaque_profile.consume_delta = opaque_profile.ring_state_offset = 0x70000000;
  control_ctx_stub.firmware = &opaque_profile;
#endif
  control_ctx_stub.pid = 4242;
  control_ctx_stub.scratch = SCRATCH_BASE;
  control_ctx_stub.scratch_len = 0x1000;
  control_ctx_stub.attached = 1;
  return &control_ctx_stub;
}
int pbs_ps4_control_ctx_release(void) { return 0; }
intptr_t pbs_ps4_control_last_scratch(void) { return SCRATCH_BASE; }
int pbs_inject_ensure_stopped(pbs_inject_t *ctx) { (void)ctx; return 0; }
int pbs_inject_revalidate_scratch(pbs_inject_t *ctx) { (void)ctx; return 0; }
int pbs_inject_resume(pbs_inject_t *ctx) { (void)ctx; return 0; }
int pbs_inject_target_rsp(pbs_inject_t *ctx, intptr_t *rsp) {
  (void)ctx; *rsp = 0; return -1;
}

int kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle) {
  (void)pid;
  if (native_module_enabled &&
      (!strcmp(basename, "remoteplay.elf") || !strcmp(basename, "SceRemotePlay") ||
       !strcmp(basename, "SceRemotePlay.elf") || !strcmp(basename, "remoteplay"))) {
    *handle = 1;
    return 0;
  }
  return -1;
}

intptr_t kernel_dynlib_mapbase_addr(pid_t pid, uint32_t handle) {
  (void)pid;
  return native_module_enabled && handle == 1 ? RP_BASE : 0;
}

/* The module-table probe is read-only diagnostics; the stub reports no table. */
typedef struct pbs_dynlib_obj {
  uintptr_t next;
  uintptr_t path;
  uint64_t unknown0[2];
  uint32_t refcount;
  uint64_t handle;
  uintptr_t mapbase;
  uintptr_t mapsize;
  uintptr_t textsize;
  uintptr_t database;
  size_t datasize;
} pbs_dynlib_obj_t;

int kernel_dynlib_obj(pid_t pid, uint32_t handle, pbs_dynlib_obj_t *obj) {
  (void)pid; (void)handle; (void)obj;
  return -1;
}

int kernel_copyout(intptr_t address, void *data, size_t size) {
  (void)address; (void)data; (void)size;
  return -1;
}

intptr_t pbs_resolve_in(pid_t pid, const char *module, const char *symbol) {
  (void)pid;
#if PBS_PS4_EXPORTED_READ
  if (!strcmp(symbol, "sceVshAvcapReadVideo")) return 0x4e00;
  if (!strcmp(symbol, "malloc")) return 0x4f00;
  if (!strcmp(symbol, "free")) return 0x5000;
#endif
  if (native_module_enabled &&
      (!strcmp(module, "remoteplay.elf") || !strcmp(module, "SceRemotePlay") ||
       !strcmp(module, "SceRemotePlay.elf") || !strcmp(module, "remoteplay")) &&
      (!strcmp(symbol, "nn_socket") || !strcmp(symbol, "nJ3r2-TtBSU#v#A")))
    return RP_NN_SOCKET;
  if (!strcmp(symbol, "sceVshAvcapInit")) return INIT_FN;
  if (!strcmp(symbol, "sceVshAvcapOpenVideoEx")) return OPEN_FN;
  if (!strcmp(symbol, "sceVshAvcapStart")) return START_FN;
  if (!strcmp(symbol, "sceVshAvcapStop")) return STOP_FN;
  if (!strcmp(symbol, "sceVshAvcapClose")) return CLOSE_FN;
  if (!strcmp(symbol, "sceVshAvcapGetInfo")) return INFO_FN;
  if (!strcmp(symbol, "sceVshAvcapSetInfo")) return SETINFO_FN;
  if (!strcmp(symbol, "sceVshAvcapGetStaticInfo")) return STATIC_FN;
  if (!strcmp(symbol, "usleep")) return IDLE_FN;
  if (!strcmp(symbol, "sceShellCoreUtilGetRemotePlayStatus"))
    return !strcmp(module, "libSceSystemService.sprx") ? STATUS_FN : 0;
  if (!lifecycle_missing && !strcmp(module, "libSceSystemService.sprx")) {
    if (!strcmp(symbol, "sceShellCoreUtilAcquireRemotePlayCpuBudget")) return ACQUIRE_FN;
    if (!strcmp(symbol, "sceShellCoreUtilPreNotifyOfRemotePlay")) return PRENOTIFY_FN;
    if (!strcmp(symbol, "sceShellCoreUtilSetRemotePlayStatus")) return SETSTATUS_FN;
    if (!strcmp(symbol, "sceShellCoreUtilReleaseRemotePlayCpuBudget")) return RELEASE_FN;
  }
  return 0;
}

void pbs_inject_end(pbs_inject_t *ctx) { ctx->attached = 0; }

int pbs_inject_read_at(pbs_inject_t *ctx, intptr_t addr, void *out, size_t len) {
  (void)ctx;
  if (fail_metadata && addr == SCRATCH_BASE + 0x300 && len == 0x40) return -1;
  if (addr < 0 || (size_t)addr + len > sizeof target) return -1;
  memcpy(out, target + addr, len);
  if (mutate_after_read_address && addr == mutate_after_read_address) {
    uint32_t generation;
    memcpy(&generation, target + mutate_header_address + 12, sizeof generation);
    generation += 2;
    memcpy(target + mutate_header_address + 12, &generation, sizeof generation);
    mutate_after_read_address = 0;
  }
  return 0;
}

int pbs_inject_write_at(pbs_inject_t *ctx, intptr_t addr, const void *in, size_t len) {
  (void)ctx;
  if (addr < 0 || (size_t)addr + len > sizeof target) return -1;
  memcpy(target + addr, in, len);
  return 0;
}

int pbs_inject_read(pbs_inject_t *ctx, void *out, size_t len) {
  return pbs_inject_read_at(ctx, ctx->scratch, out, len);
}

int pbs_inject_write(pbs_inject_t *ctx, const void *in, size_t len) {
  return pbs_inject_write_at(ctx, ctx->scratch, in, len);
}

#if PBS_PS4_WORKER_BOOT
/* Host valve for the dedicated worker: converges the boot and detaches like
 * the real pthread/loopback boot does, so the capture continues against the
 * mocked target without any extra transport. */
int pbs_ps4_worker_boot(pbs_inject_t *ctx, FILE *log) {
  assert(log && ctx->attached);
  ctx->attached = 0;
  ctx->worker = 1;
  return 0;
}
#endif

static uint16_t last_ready_dimensions[4];

static long call(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]) {
  seen_trap = ctx->trap;
  switch ((uintptr_t)fn) {
#if PBS_PS4_EXPORTED_READ
    case 0x4f00:
      assert(ctx->worker && args[0] == 0x200000);
      return FRAME_ADDR;
    case 0x5000:
      assert(args[0] == FRAME_ADDR);
      ++exported_frees;
      return 0;
    case 0x4e00:
      assert(ctx->worker && args[0] == 0x12345678u);
      assert(args[1] == FRAME_ADDR && args[2] == FRAME_LEN && args[4] == 1);
      ++exported_reads;
      for (unsigned i = 0; i < FRAME_LEN; ++i)
        target[FRAME_ADDR + i] = (unsigned char)(i + 1);
      memset(target + args[3], 0x5a, 0x40);
      return FRAME_LEN;
#endif
    case STATUS_FN:
      target[(size_t)args[0]] = native_status;
      return 0;
    case ACQUIRE_FN:
      ++lifecycle_acquires;
      lifecycle_budget = 1;
      return 0;
    case PRENOTIFY_FN:
      if (lifecycle_fail_pre == 2) return LONG_MIN;
      return lifecycle_fail_pre ? (long)0x80e30003u : 0;
    case SETSTATUS_FN:
      ++lifecycle_sets;
      if (!args[0] && lifecycle_fail_clear) return (long)0x80e30003u;
      native_status = (unsigned char)args[0];
      return 0;
    case RELEASE_FN:
      ++lifecycle_releases;
      lifecycle_budget = 0;
      return 0;
    case IDLE_FN:
      assert(args[0] == 10000);
      ++idle_calls;
      return fail_idle ? LONG_MIN : 0;
    case INIT_FN:
      return 0;
    case READY_FN:
      memcpy(last_ready_dimensions, target + (size_t)args[0] + 4, sizeof last_ready_dimensions);
      ++ready_calls;
      return ready_calls < 3 ? (long)0x80a70008u : 0;
    case DEQUEUE_FN: {
      ++dequeue_calls;
      if (dequeue_result != FRAME_LEN) return dequeue_result;
      uint64_t payload = FRAME_ADDR;
      unsigned char metadata[0x40];
      memset(metadata, 0x5a, sizeof metadata);
      for (unsigned i = 0; i < FRAME_LEN; ++i)
        target[FRAME_ADDR + i] = (unsigned char)(i + 1);
      pbs_inject_write_at(ctx, (intptr_t)args[3], &payload, sizeof payload);
      pbs_inject_write_at(ctx, (intptr_t)args[1], metadata, sizeof metadata);
      return FRAME_LEN;
    }
    case CONSUME_FN:
      ++consume_calls;
      return consume_result;
    case OPEN_FN: {
      uint32_t usage = 0, profile = 0;
      pbs_inject_read_at(ctx, (intptr_t)args[1] + 0x04, &usage, sizeof usage);
      pbs_inject_read_at(ctx, (intptr_t)args[1] + 0x08, &profile, sizeof profile);
      request_word_usage = usage;
      request_word_profile = profile;
      ++open_calls;
#if PBS_PS4_ADMISSION_PROBE || PBS_PS4_REMOTE_1080
      if (forced_open_error) return forced_open_error;
#endif
      if (open_calls < 3) return (long)0x80a70008u;
      uint64_t handle = 0x12345678u;
      pbs_inject_write_at(ctx, (intptr_t)args[0], &handle, sizeof handle);
      return 0;
    }
    case START_FN:
      ++start_calls;
      return fail_start ? LONG_MIN : 0;
    case STOP_FN:
      ++stop_calls;
      return 0;
    case CLOSE_FN:
      ++close_calls;
      return 0;
    case INFO_FN: {
      ++info_calls;
      unsigned char *out = target + (intptr_t)args[2];
      size_t size = (size_t)args[3];
      if ((uint32_t)args[1] == 0) {
        uint32_t value = 0x1f;
        assert(size >= sizeof value);
        memcpy(out, &value, sizeof value);
        return 0;
      }
      if ((uint32_t)args[1] == 1) {
        static const unsigned char sps[] = {0x67, 0x64, 0x00, 0x1f};
        if (size < sizeof sps) return -1;
        memcpy(out, sps, sizeof sps);
        return (long)sizeof sps;
      }
      static const unsigned char pps[] = {0x68, 0xeb, 0xe3, 0xcb};
      if (size < sizeof pps) return -1;
      memcpy(out, pps, sizeof pps);
      return (long)sizeof pps;
    }
    case SETINFO_FN:
      return 0;
    case STATIC_FN: {
      ++static_calls;
      uint32_t value = (uint32_t)args[0] == 24 ? 1u : 0x00440008u;
      pbs_inject_write_at(ctx, (intptr_t)args[1], &value, sizeof value);
      return 4;
    }
    case NATIVE_ENSURE_FN: {
      intptr_t inner = (intptr_t)args[0];
      assert(inner == NATIVE_INNER);
      uint64_t handle = 0x12345678u;
      pbs_inject_write_at(ctx, inner + 0x68, &handle, sizeof handle);
      pbs_inject_write_at(ctx, inner + 0x90, &handle, sizeof handle);
      return 1;
    }
    default:
      return -1;
  }
}

long pbs_inject_call(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]) {
  return call(ctx, fn, args);
}
long pbs_inject_call_fast(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]) {
  return call(ctx, fn, args);
}
long pbs_inject_call_fast_fp(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6], float xmm0) {
  (void)xmm0;
  return call(ctx, fn, args);
}

static void run_native_test(FILE *log) {
  setup_native_module();
  native_module_enabled = 1;
  reset_counters();
  assert(pbs_ps4_remote_capture_begin(log) == 0);
  assert(pbs_ps4_remote_capture_init() == 0);
  assert(seen_trap == pbs_ps4_firmware_trap(pbs_ps4_firmware_find(0x11008001u), INIT_FN));
  uint32_t request[PBS_PS4_VIDEO_REQUEST_WORDS];
  assert(pbs_ps4_make_video_request(request, 720, 0, 0) == 0);
  void *handle = NULL;
  assert(pbs_ps4_remote_open(&handle, request) == 0);
  assert(handle == (void *)0x12345678u);
  assert(open_calls == 0);
  assert(pbs_ps4_remote_start(handle) == 0);
  assert(start_calls == 0);
  unsigned char frame[FRAME_LEN], info[0x40];
  memset(frame, 0, sizeof frame);
  memset(info, 0, sizeof info);
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == FRAME_LEN);
  assert(dequeue_calls == 1 && consume_calls == 1);
  assert(pbs_ps4_remote_stop(handle) == 0 && stop_calls == 1);
  assert(pbs_ps4_remote_close(handle) == 0 && close_calls == 1);
  assert(pbs_ps4_remote_capture_end() == 0);
  assert(pbs_ps4_remote_capture_active() == 0);
}

int main(void) {
  FILE *log = tmpfile();
  assert(log);
  /* The native ensure-open call is an opt-in experiment (see the kernel-panic
   * note in platform/ps4/capture/remote.c); only exercise it when the backend was
   * compiled with it enabled. */
#if PBS_PS4_NATIVE_ENSURE_OPEN
  run_native_test(log);
#else
  (void)run_native_test;
#endif
  native_module_enabled = 0;
  reset_counters();
  assert(pbs_ps4_remote_capture_begin(log) == 0);
  assert(pbs_ps4_remote_capture_init() == 0);
  assert(pbs_ps4_remote_capture_active() == 1);
  assert(static_calls >= 2);

  uint32_t request[PBS_PS4_VIDEO_REQUEST_WORDS];
  assert(pbs_ps4_make_video_request(request, 720, 0, 0) == 0);
  void *handle = NULL;
  assert(pbs_ps4_remote_open(&handle, request) == 0);
  assert(handle == (void *)0x12345678u);
  assert(ready_calls >= 3);
  assert(open_calls == 3);
  assert(request_word_usage == 0);
  assert(request_word_profile == 19);

  unsigned char sps[128], pps[128];
  int sps_len = pbs_ps4_remote_info(handle, 1, sps, sizeof sps);
  int pps_len = pbs_ps4_remote_info(handle, 2, pps, sizeof pps);
  assert(sps_len == 4 && pps_len == 4 && info_calls == 2);
  assert(sps[0] == 0x67 && pps[0] == 0x68);
  uint32_t fixed_info = 0;
  assert(pbs_ps4_remote_info(handle, 0, &fixed_info, sizeof fixed_info) == 0);
  assert(fixed_info == 0x1f);

  assert(pbs_ps4_remote_start(handle) == 0 && start_calls == 1);
  assert(pbs_ps4_remote_keyframe(handle) == 0);
  unsigned char frame[FRAME_LEN], info[0x40];
  memset(frame, 0, sizeof frame);
  memset(info, 0, sizeof info);
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == FRAME_LEN);
  assert(dequeue_calls == 1 && consume_calls == 1);
  for (unsigned i = 0; i < FRAME_LEN; ++i) assert(frame[i] == (unsigned char)(i + 1));
  for (unsigned i = 0; i < 0x40; ++i) assert(info[i] == 0x5a);

  /* Both RAX extension conventions occur with 32-bit Orbis status codes.
   * Errors must never consume a frame or look like multi-GiB packet sizes. */
  const long ring_status[] = {
    0x80a70002L, (long)(int32_t)0x80a70002u,
    0x80a70004L, (long)(int32_t)0x80a70004u,
    0x80bf0011L, (long)(int32_t)0x80bf0011u, PBS_CALL_TIMEOUT
  };
  for (unsigned i = 0; i < sizeof ring_status / sizeof ring_status[0]; ++i) {
    int before_consume = consume_calls;
    dequeue_result = ring_status[i];
    int rc = pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1);
    assert(i < 4 ? rc == 0 : rc < 0);
    assert(consume_calls == before_consume);
  }
  dequeue_result = FRAME_LEN;
  consume_result = PBS_CALL_TIMEOUT;
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) < 0);
  consume_result = 0x80a70001L;
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) < 0);
  consume_result = 0;
  fail_metadata = 1;
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) < 0);
  fail_metadata = 0;
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == FRAME_LEN);

  /* A timed-out remote call must surface as a negative error, never zero. */
  fail_start = 1;
  assert(pbs_ps4_remote_start(handle) == -1);
  fail_start = 0;

  assert(pbs_ps4_remote_stop(handle) == 0 && stop_calls == 1);
  assert(pbs_ps4_remote_close(handle) == 0 && close_calls == 1);
  assert(pbs_ps4_remote_capture_end() == 0);
  assert(pbs_ps4_remote_capture_active() == 0);

  /* 1080p uses the same ownership path, with a 1920x1080 readiness surface
   * and native profile 20 rather than advertising a larger HTTP size only. */
  reset_counters();
  assert(pbs_ps4_remote_capture_init() == 0);
  assert(pbs_ps4_make_video_request(request, 1080, 0, 0) == 0);
  handle = NULL;
  assert(pbs_ps4_remote_open(&handle, request) == 0);
  assert(request_word_usage == 0 && request_word_profile == 20);
  assert(last_ready_dimensions[0] == 1920 && last_ready_dimensions[1] == 1080);
  assert(last_ready_dimensions[2] == 1920 && last_ready_dimensions[3] == 1080);
  assert(pbs_ps4_remote_start(handle) == 0);
  assert(pbs_ps4_remote_stop(handle) == 0);
  assert(pbs_ps4_remote_close(handle) == 0);
  assert(pbs_ps4_remote_capture_end() == 0);

  reset_counters();
  assert(pbs_ps4_remote_capture_init() == 0);
  assert(pbs_ps4_make_video_request_at_rate(request, 1080, 0, 0, 30) == 0);
  handle = NULL;
  assert(pbs_ps4_remote_open(&handle, request) == 0);
  assert(request_word_profile == 2 && request_word_usage == 0);
  assert(last_ready_dimensions[0] == 1920 && last_ready_dimensions[1] == 1080);
  assert(pbs_ps4_remote_stop(handle) == 0 && pbs_ps4_remote_close(handle) == 0);
  assert(pbs_ps4_remote_capture_end() == 0);

#if !PBS_PS4_ADMISSION_PROBE
  reset_counters();
  assert(!pbs_ps4_remote_capture_init());
  assert(!pbs_ps4_make_video_request_remote_1080(request,1080,0,0,30));
  handle=NULL;
#if PBS_PS4_REMOTE_1080
  snapshot_fail=1;
  assert(pbs_ps4_remote_open(&handle,request)<0 && !handle && !open_calls);
  snapshot_fail=0;
#endif
  assert(!pbs_ps4_remote_open(&handle,request));
  assert(request_word_profile==24 && request_word_usage==0 && handle);
  assert(last_ready_dimensions[0]==1920 && last_ready_dimensions[1]==1080);
  assert(last_ready_dimensions[2]==1920 && last_ready_dimensions[3]==1080);
  assert(!pbs_ps4_remote_start(handle));
  assert(!pbs_ps4_remote_stop(handle) && !pbs_ps4_remote_close(handle));
  assert(!pbs_ps4_remote_capture_end());
#endif

#if PBS_PS4_ADMISSION_PROBE
  reset_counters();
  assert(pbs_ps4_remote_capture_init() == 0);
  handle = NULL;
  snapshot_fail = 1;
  assert(pbs_ps4_remote_open(&handle, request) < 0);
  assert(!handle && !open_calls && !ready_calls);
  snapshot_fail = 0;
  forced_open_error = 0x80a7000a;
  int snapshots_before = snapshot_calls;
  assert((uint32_t)pbs_ps4_remote_open(&handle, request) == forced_open_error);
  assert(!handle && open_calls == 1 && snapshot_calls == snapshots_before + 2);
  assert(pbs_ps4_remote_capture_end() == 0);
#endif
  fclose(log);
  puts("PS4 remote Avcap injected backend: request, readiness, SPS/PPS, ring copy/consume and cleanup verified");
  return 0;
}
