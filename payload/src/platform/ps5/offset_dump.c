// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset_dump.h"
#include <ps5/kernel.h>
#include <inttypes.h>

typedef struct {
  const char *module;
  const char *const *symbols;
  size_t count;
} module_exports;

static const char *const avcap2_exports[] = {
  "sceAvcap2Initialize", "sceAvcap2OpenVideo", "sceAvcap2GetVideoOutMode",
  "sceAvcap2EnableScanInEncode", "sceAvcap2Start", "sceAvcap2ReadVideo",
  "sceAvcap2GetNonVclNalUnitStream", "sceAvcap2Close", "sceAvcap2OpenAudio"
};
static const char *const arbitrator_exports[] = {
  "sceResourceArbitratorSetCallback", "sceResourceArbitratorChangeState",
  "sceResourceArbitratorClearCallback"
};

static const char *const capture_exports[] = {
  "sceAvcap2Initialize", "sceAvcap2Terminate", "sceAvcap2OpenVideo",
  "sceAvcap2OpenAudio", "sceAvcap2GetVideoOutMode",
  "sceAvcap2EnableScanInEncode", "sceAvcap2Start", "sceAvcap2Stop",
  "sceAvcap2Close", "sceAvcap2ReadVideo", "sceAvcap2ReadAudio",
  "sceAvcap2GetNonVclNalUnitStream", "sceAvcap2RequestKeyFrame",
  "sceAvcap2SetVideoBitrate"
};
static const char *const libc_exports[] = {"malloc", "free"};
static const char *const kernel_exports[] = {"getpid"};
static const char *const user_exports[] = {"sceUserServiceGetInitialUser"};

void pbs_ps5_dump_dependencies(FILE *report, pbs_inject_t *ctx) {
  if (!report || !ctx || ctx->pid <= 0) return;
  const module_exports modules[] = {
    {"libSceAvcap2.sprx", capture_exports, sizeof capture_exports / sizeof capture_exports[0]},
    {"libSceResourceArbitrator.sprx", arbitrator_exports,
     sizeof arbitrator_exports / sizeof arbitrator_exports[0]},
    {"libSceLibcInternal.sprx", libc_exports, sizeof libc_exports / sizeof libc_exports[0]},
    {"libkernel_sys.sprx", kernel_exports, sizeof kernel_exports / sizeof kernel_exports[0]},
    {"libSceUserService.sprx", user_exports, sizeof user_exports / sizeof user_exports[0]}
  };
  fprintf(report, "dependency dump: firmware=0x%08x target_pid=%d; export fingerprints, not private ABI offsets\n",
          kernel_get_fw_version(), (int)ctx->pid);
  for (size_t m = 0; m < sizeof modules / sizeof modules[0]; ++m) {
    uint32_t handle = 0;
    if (kernel_dynlib_handle(ctx->pid, modules[m].module, &handle)) {
      fprintf(report, "dependency dump: module=%s absent\n", modules[m].module);
      continue;
    }
    intptr_t base = kernel_dynlib_mapbase_addr(ctx->pid, handle);
    if (base <= 0) {
      fprintf(report, "dependency dump: module=%s base unavailable\n", modules[m].module);
      continue;
    }
    fprintf(report, "dependency dump: module=%s base=0x%" PRIxPTR " handle=%u\n",
            modules[m].module, (uintptr_t)base, handle);
    for (size_t i = 0; i < modules[m].count; ++i) {
      const char *name = modules[m].symbols[i];
      intptr_t address = kernel_dynlib_dlsym(ctx->pid, handle, name);
      if (address < base || (uintptr_t)(address - base) >= 0x4000000u) {
        fprintf(report, "dependency dump: %s unresolved\n", name);
        continue;
      }
      /* The detached worker executes inside the target.  Some firmwares map
       * module text execute-only: reading those bytes from the worker can
       * fault its thread and destroy the RPC channel needed for STOP and
       * Terminate.  RVAs remain safe and sufficient for postmortem matching;
       * byte fingerprints are collected only while ptrace serves reads. */
      if (ctx->worker) {
        fprintf(report, "dependency dump: %s +0x%" PRIxPTR
                        " code-read-skipped-worker\n",
                name, (uintptr_t)(address - base));
        continue;
      }
      unsigned char bytes[32];
      if (pbs_inject_read_at(ctx, address, bytes, sizeof bytes)) {
        fprintf(report, "dependency dump: %s +0x%" PRIxPTR " code-read-failed\n",
                name, (uintptr_t)(address - base));
        continue;
      }
      uint64_t fingerprint = UINT64_C(14695981039346656037);
      for (size_t j = 0; j < sizeof bytes; ++j) {
        fingerprint ^= bytes[j];
        fingerprint *= UINT64_C(1099511628211);
      }
      fprintf(report, "dependency dump: %s +0x%" PRIxPTR " code32_fnv64=0x%016" PRIx64 " first8=",
              name, (uintptr_t)(address - base), fingerprint);
      for (size_t j = 0; j < 8; ++j) fprintf(report, "%02x", bytes[j]);
      fputc('\n', report);
    }
  }
}

void pbs_ps5_dump_initial_user(FILE *report, pbs_inject_t *ctx) {
  if (!report || !ctx) return;
  intptr_t fn = pbs_resolve_in(ctx->pid, "libSceUserService.sprx",
                                "sceUserServiceGetInitialUser");
  if (!fn) {
    fprintf(report, "dependency dump: initial-user getter unavailable; account activation unknown\n");
    return;
  }
  uint32_t empty = 0;
  intptr_t slot = ctx->scratch + 0x740;
  if (pbs_inject_write_at(ctx, slot, &empty, sizeof empty)) {
    fprintf(report, "dependency dump: initial-user scratch write failed\n");
    return;
  }
  long rc = pbs_inject_call_fast(ctx, fn, (uint64_t[6]){(uint64_t)slot, 0, 0, 0, 0, 0});
  uint32_t user_id = 0;
  if (rc == LONG_MIN || ctx->asserted ||
      pbs_inject_read_at(ctx, slot, &user_id, sizeof user_id)) {
    fprintf(report, "dependency dump: GetInitialUser unavailable after call; account activation unknown\n");
    return;
  }
  fprintf(report, "dependency dump: GetInitialUser rc=0x%08x user_id=0x%08x; PSN activation not measured\n",
          (unsigned)(uint32_t)rc, user_id);
}

void pbs_ps5_dump_export_offsets(FILE *report, pid_t pid) {
  if (!report || pid <= 0) return;
  const module_exports modules[] = {
    {"libSceAvcap2.sprx", avcap2_exports,
     sizeof avcap2_exports / sizeof avcap2_exports[0]},
    {"libSceResourceArbitrator.sprx", arbitrator_exports,
     sizeof arbitrator_exports / sizeof arbitrator_exports[0]}
  };
  fprintf(report, "offset dump: firmware=0x%08x pid=%d; export RVAs only; no firmware field inference\n",
          kernel_get_fw_version(), (int)pid);
  for (size_t m = 0; m < sizeof modules / sizeof modules[0]; ++m) {
    uint32_t handle = 0;
    if (kernel_dynlib_handle(pid, modules[m].module, &handle)) {
      fprintf(report, "offset dump: %s unavailable\n", modules[m].module);
      continue;
    }
    intptr_t base = kernel_dynlib_mapbase_addr(pid, handle);
    if (base <= 0) {
      fprintf(report, "offset dump: %s invalid mapping base\n", modules[m].module);
      continue;
    }
    fprintf(report, "offset dump: %s base=0x%" PRIxPTR " handle=%u\n",
            modules[m].module, (uintptr_t)base, handle);
    for (size_t i = 0; i < modules[m].count; ++i) {
      intptr_t address = kernel_dynlib_dlsym(pid, handle, modules[m].symbols[i]);
      if (address < base || (uintptr_t)(address - base) >= 0x4000000u) {
        fprintf(report, "offset dump: %s unresolved/outside bounded RVA\n",
                modules[m].symbols[i]);
      } else {
        fprintf(report, "offset dump: %s +0x%" PRIxPTR "\n",
                modules[m].symbols[i], (uintptr_t)(address - base));
      }
    }
  }
}

void pbs_ps5_dump_request(FILE *report, const char *phase,
                          const unsigned char *request, size_t length) {
  if (!report || !phase || !request || length > 0x400) return;
  fprintf(report, "offset dump: %s request bytes=%zu\n", phase, length);
  for (size_t offset = 0; offset < length; offset += 16) {
    fprintf(report, "  +%03zx:", offset);
    size_t end = length - offset < 16 ? length : offset + 16;
    for (size_t i = offset; i < end; ++i) fprintf(report, " %02x", request[i]);
    fputc('\n', report);
  }
}

void pbs_ps5_dump_request_changes(FILE *report, const unsigned char *before,
                                  const unsigned char *after, size_t length) {
  if (!report || !before || !after || length > 0x400) return;
  size_t changed = 0;
  for (size_t offset = 0; offset < length; ++offset) {
    if (before[offset] == after[offset]) continue;
    fprintf(report, "offset dump: request +0x%03zx %02x -> %02x\n",
            offset, before[offset], after[offset]);
    ++changed;
  }
  fprintf(report, "offset dump: changed request bytes=%zu\n", changed);
}
