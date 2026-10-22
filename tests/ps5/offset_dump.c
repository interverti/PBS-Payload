// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset_dump.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t kernel_get_fw_version(void) { return 0x12700000u; }

int kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle) {
  assert(pid == 42);
  *handle = !strcmp(basename, "libSceAvcap2.sprx") ? 1u :
            !strcmp(basename, "libSceResourceArbitrator.sprx") ? 2u :
            !strcmp(basename, "libSceLibcInternal.sprx") ? 3u :
            !strcmp(basename, "libkernel_sys.sprx") ? 4u : 5u;
  return 0;
}
intptr_t kernel_dynlib_mapbase_addr(pid_t pid, uint32_t handle) {
  assert(pid == 42);
  return (intptr_t)handle * 0x100000;
}
intptr_t kernel_dynlib_dlsym(pid_t pid, uint32_t handle, const char *symbol) {
  assert(pid == 42);
  if (!strcmp(symbol, "sceAvcap2OpenVideo")) return 0x101234;
  if (!strcmp(symbol, "sceResourceArbitratorChangeState")) return 0x1fffff;
  return (intptr_t)handle * 0x100000 + 0x80;
}
int pbs_inject_read_at(pbs_inject_t *ctx, intptr_t address, void *out, size_t length) {
  assert(ctx->pid == 42 && length <= 32);
  memset(out, (unsigned char)address, length);
  return 0;
}
int pbs_inject_write_at(pbs_inject_t *ctx, intptr_t address, const void *in, size_t length) {
  assert(ctx->pid == 42 && address == ctx->scratch + 0x740 && length == 4);
  (void)in;
  return 0;
}
intptr_t pbs_resolve_in(pid_t pid, const char *module, const char *symbol) {
  assert(pid == 42 && !strcmp(module, "libSceUserService.sprx"));
  assert(!strcmp(symbol, "sceUserServiceGetInitialUser"));
  return 0x500080;
}
long pbs_inject_call_fast(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]) {
  assert(ctx->pid == 42 && fn == 0x500080 && args[0] == (uint64_t)ctx->scratch + 0x740);
  return 0;
}

int main(void) {
  FILE *report = tmpfile();
  assert(report);
  pbs_ps5_dump_export_offsets(report, 42);
  pbs_inject_t ctx = {.pid = 42, .scratch = 0x800000};
  pbs_ps5_dump_dependencies(report, &ctx);
  pbs_ps5_dump_initial_user(report, &ctx);
  unsigned char request[17] = {0};
  request[0] = 0x7f;
  request[16] = 0x42;
  pbs_ps5_dump_request(report, "before OpenVideo", request, sizeof request);
  unsigned char after[17];
  memcpy(after, request, sizeof after);
  after[16] = 0x43;
  pbs_ps5_dump_request_changes(report, request, after, sizeof after);
  assert(!fseek(report, 0, SEEK_END));
  long length = ftell(report);
  assert(length > 0 && length < 16384);
  assert(!fseek(report, 0, SEEK_SET));
  char *text = malloc((size_t)length + 1);
  assert(text);
  assert(fread(text, 1, (size_t)length, report) == (size_t)length);
  text[length] = 0;
  assert(strstr(text, "sceAvcap2OpenVideo +0x1234"));
  assert(strstr(text, "firmware=0x12700000 pid=42"));
  assert(strstr(text, "sceResourceArbitratorChangeState unresolved/outside bounded RVA"));
  assert(strstr(text, "  +000: 7f"));
  assert(strstr(text, "  +010: 42"));
  assert(strstr(text, "request +0x010 42 -> 43"));
  assert(strstr(text, "changed request bytes=1"));
  assert(strstr(text, "module=libSceLibcInternal.sprx"));
  assert(strstr(text, "sceAvcap2SetVideoBitrate +0x80 code32_fnv64="));
  assert(strstr(text, "GetInitialUser rc=0x00000000"));
  free(text);
  fclose(report);
  return 0;
}
