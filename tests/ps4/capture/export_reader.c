// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/capture/export_reader.h"
#include "platform/ps4/worker/remote.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BUFFER 0x20000
#define SCRATCH 0x1000
static unsigned char memory[0x240000], output[0x200000];
static int missing, allocation_failure, timeout_fn, calls, frees, copies;
static long read_result;
static size_t seen_capacity;
static uint64_t seen_flags;

intptr_t pbs_resolve_in(pid_t pid, const char *module, const char *symbol) {
  assert(pid == 123);
  if (!strcmp(symbol, "sceVshAvcapReadVideo")) {
    assert(!strcmp(module, "libSceAvcap.sprx"));
    return missing ? 0 : 1;
  }
  assert(!strcmp(module, "libSceLibcInternal.sprx"));
  if (!strcmp(symbol, "malloc")) return 2;
  assert(!strcmp(symbol, "free"));
  return 3;
}

long pbs_inject_call_fast(pbs_inject_t *ctx, intptr_t fn, const uint64_t a[6]) {
  assert(ctx->worker && !ctx->asserted);
  ++calls;
  if (fn == timeout_fn) return PBS_CALL_TIMEOUT;
  if (fn == 2) { assert(a[0] == sizeof output); return allocation_failure ? 0 : BUFFER; }
  if (fn == 3) { assert(a[0] == BUFFER); ++frees; return 0; }
  assert(fn == 1 && a[0] == 0xdead && a[1] == BUFFER);
  assert(a[3] == SCRATCH + 0x300 && a[5] == 0);
  seen_capacity = a[2]; seen_flags = a[4];
  memset(memory + SCRATCH + 0x300, 0x5a, 0x40);
  return read_result;
}

int pbs_inject_read_at(pbs_inject_t *ctx, intptr_t addr, void *out, size_t len) {
  (void)ctx;
  assert(addr >= 0 && (size_t)addr + len <= sizeof memory);
  assert(len <= PBS_WORKER_COPY_MAX);
  ++copies;
  memcpy(out, memory + addr, len);
  return 0;
}
int pbs_inject_write_at(pbs_inject_t *ctx, intptr_t addr, const void *in, size_t len) {
  (void)ctx;
  assert(addr == SCRATCH + 0x300 && len == 0x40);
  memcpy(memory + addr, in, len);
  return 0;
}

int main(void) {
  pbs_inject_t ctx = {.pid=123, .scratch=SCRATCH, .scratch_len=4096};
  pbs_ps4_export_reader r = {0};
  assert(pbs_ps4_export_reader_init(&r, &ctx) < 0 && calls == 0);
  ctx.worker = 1;
  missing = 1;
  assert(pbs_ps4_export_reader_init(&r, &ctx) < 0 && calls == 0);
  missing = 0; allocation_failure = 1;
  assert(pbs_ps4_export_reader_init(&r, &ctx) < 0 && !r.buffer);
  allocation_failure = 0;
  assert(!pbs_ps4_export_reader_init(&r, &ctx));
  for (size_t i=0; i<sizeof output; ++i) memory[BUFFER+i] = (unsigned char)(i*13);
  unsigned char meta[0x40];
  read_result = PBS_WORKER_COPY_MAX + 17;
  assert(pbs_ps4_export_reader_read(&r, &ctx, (void *)0xdead, output,
                                   sizeof output + 17, meta, 0) < 0);
  assert(seen_capacity == sizeof output && copies == 0);
  read_result = PBS_WORKER_COPY_MAX;
  assert(pbs_ps4_export_reader_read(&r, &ctx, (void *)0xdead, output,
                                   sizeof output, meta, 0) == read_result);
  assert(seen_flags == 1 && seen_capacity == sizeof output && copies == 2);
  assert(!memcmp(output, memory+BUFFER, (size_t)read_result));
  for (unsigned i=0; i<sizeof meta; ++i) assert(meta[i] == 0x5a);
  int previous_copies = copies;
  read_result = 0x80a70002u;
  assert(!pbs_ps4_export_reader_read(&r, &ctx, (void *)0xdead, output, 100, meta, 1));
  read_result = 0x80a70004u;
  assert((uint32_t)pbs_ps4_export_reader_read(&r, &ctx, (void *)0xdead, output, 100, meta, 1) == 0x80a70004u);
  read_result = 101;
  assert(pbs_ps4_export_reader_read(&r, &ctx, (void *)0xdead, output, 100, meta, 1) < 0);
  assert(copies == previous_copies && seen_capacity == 100);
  assert(!pbs_ps4_export_reader_release(&r, &ctx) && frees == 1);
  assert(!pbs_ps4_export_reader_release(&r, &ctx) && frees == 1);
  assert(!pbs_ps4_export_reader_init(&r, &ctx));
  timeout_fn = 1;
  assert(pbs_ps4_export_reader_read(&r, &ctx, (void *)0xdead, output, 100, meta, 1) < 0);
  assert(ctx.asserted && pbs_ps4_export_reader_release(&r, &ctx) < 0 && frees == 1);
  int previous_calls = calls;
  assert(pbs_ps4_export_reader_read(&r, &ctx, (void *)0xdead, output, 100, meta, 1) < 0);
  assert(calls == previous_calls);
  puts("Exported ReadVideo: opaque handle, frame capacity, bounds, errors, release and timeout quarantine verified");
  return 0;
}
