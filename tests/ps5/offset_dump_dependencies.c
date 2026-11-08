// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset_dump.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ps5/kernel.h>

static unsigned code_reads;

uint32_t kernel_get_fw_version(void) { return 0x05500008u; }

int kernel_dynlib_handle(pid_t pid, const char *name, uint32_t *handle) {
  assert(pid == 60 && name && handle);
  *handle = (uint32_t)(unsigned char)name[3] + 1u;
  return 0;
}

intptr_t kernel_dynlib_mapbase_addr(pid_t pid, uint32_t handle) {
  assert(pid == 60 && handle);
  return (intptr_t)0x800000000u + (intptr_t)handle * 0x100000;
}

intptr_t kernel_dynlib_dlsym(pid_t pid, uint32_t handle, const char *symbol) {
  assert(pid == 60 && handle && symbol);
  return kernel_dynlib_mapbase_addr(pid, handle) + 0x100 +
         (intptr_t)(unsigned char)symbol[0];
}

int pbs_inject_read_at(pbs_inject_t *ctx, intptr_t address, void *output,
                       size_t length) {
  assert(ctx && ctx->pid == 60 && address > 0 && output && length == 32);
  memset(output, 0xa5, length);
  code_reads++;
  return 0;
}

intptr_t pbs_resolve_in(pid_t pid, const char *module, const char *symbol) {
  uint32_t handle = 0;
  assert(!kernel_dynlib_handle(pid, module, &handle));
  return kernel_dynlib_dlsym(pid, handle, symbol);
}

int pbs_inject_write_at(pbs_inject_t *ctx, intptr_t address,
                        const void *input, size_t length) {
  (void)ctx; (void)address; (void)input; (void)length;
  return -1;
}

long pbs_inject_call_fast(pbs_inject_t *ctx, intptr_t fn,
                          const uint64_t args[6]) {
  (void)ctx; (void)fn; (void)args;
  return PBS_CALL_TIMEOUT;
}

static void read_report(FILE *report, char *text, size_t capacity) {
  assert(!fflush(report));
  rewind(report);
  size_t used = fread(text, 1, capacity - 1, report);
  text[used] = 0;
}

int main(void) {
  char text[16384];
  pbs_inject_t ctx = {.pid = 60, .worker = 1};
  FILE *report = tmpfile();
  assert(report);
  pbs_ps5_dump_dependencies(report, &ctx);
  read_report(report, text, sizeof text);
  assert(!code_reads);
  assert(strstr(text, "code-read-skipped-worker"));
  fclose(report);

  ctx.worker = 0;
  report = tmpfile();
  assert(report);
  pbs_ps5_dump_dependencies(report, &ctx);
  read_report(report, text, sizeof text);
  assert(code_reads > 0);
  assert(strstr(text, "code32_fnv64="));
  fclose(report);
  puts("PS5 dependency dump: worker skips execute-only code reads; ptrace keeps fingerprints");
  return 0;
}
