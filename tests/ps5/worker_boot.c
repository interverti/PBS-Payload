// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/worker.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "platform/ps4/worker/client.h"
#include "platform/ps4/worker/remote.h"

#define PID 47
#define STACK_SCRATCH ((intptr_t)0x100000)
#define PRIVATE_SCRATCH UINT64_C(0x200000)
#define MAP_ADDRESS ((intptr_t)0x400000)
#define THREAD UINT64_C(0x1234)
#define FN_MMAP ((intptr_t)1)
#define FN_CREATE ((intptr_t)2)
#define FN_JOIN ((intptr_t)3)
#define FN_MUNMAP ((intptr_t)4)

static int detached, attached, protected, joined, unmapped, exited;
enum { SCENARIO_OK, SCENARIO_CREATE_TIMEOUT, SCENARIO_HANDLE_READ,
       SCENARIO_EXIT_FAILURE };
static int scenario;
static pbs_worker_call_hook call_hook;
static pbs_worker_read_hook read_hook;
static pbs_worker_write_hook write_hook;

intptr_t pbs_resolve_in(pid_t pid, const char *module, const char *symbol) {
  (void)module;
  assert(pid == PID);
  if (!strcmp(symbol, "mmap")) return FN_MMAP;
  if (!strcmp(symbol, "pthread_create")) return FN_CREATE;
  if (!strcmp(symbol, "pthread_join")) return FN_JOIN;
  if (!strcmp(symbol, "munmap")) return FN_MUNMAP;
  if (!strncmp(symbol, "sce", 3)) return 0;
  return (intptr_t)0x500000;
}

int pbs_inject_begin(const char *name, pbs_inject_t *ctx) {
  assert(!strcmp(name, "SceRemotePlay"));
  memset(ctx, 0, sizeof *ctx);
  ctx->pid = PID;
  ctx->attached = 1;
  ctx->scratch = STACK_SCRATCH;
  ctx->scratch_len = 0x1000;
  ++attached;
  return 0;
}

void pbs_inject_end(pbs_inject_t *ctx) {
  assert(ctx && ctx->attached);
  ctx->attached = 0;
  ++detached;
}

int pbs_inject_write_at(pbs_inject_t *ctx, intptr_t address, const void *input,
                        size_t length) {
  assert(ctx->attached && address >= MAP_ADDRESS && input && length);
  return 0;
}

int pbs_inject_read_at(pbs_inject_t *ctx, intptr_t address, void *output,
                       size_t length) {
  assert(ctx->attached && address == STACK_SCRATCH + 0x700 &&
         length == sizeof(uint64_t));
  if (scenario == SCENARIO_HANDLE_READ) return -1;
  memcpy(output, &(uint64_t){THREAD}, length);
  return 0;
}

long pbs_inject_call_fast(pbs_inject_t *ctx, intptr_t fn,
                          const uint64_t args[6]) {
  assert(ctx->attached);
  if (fn == FN_MMAP) {
    assert(args[0] == 0 && args[1] == 0x4000 &&
           args[2] == (PROT_READ | PROT_WRITE) &&
           args[3] == (MAP_PRIVATE | MAP_ANON) && args[4] == UINT64_MAX);
    return MAP_ADDRESS;
  }
  if (fn == FN_CREATE) {
    assert(args[0] == STACK_SCRATCH + 0x700 && args[1] == 0 &&
           args[2] == MAP_ADDRESS && args[3] > MAP_ADDRESS);
    if (scenario == SCENARIO_CREATE_TIMEOUT) return PBS_CALL_TIMEOUT;
    return 0;
  }
  if (fn == FN_JOIN) {
    assert(args[0] == THREAD && args[1] == 0);
    ++joined;
    return 0;
  }
  assert(fn == FN_MUNMAP && args[0] == MAP_ADDRESS && args[1] == 0x4000);
  ++unmapped;
  return 0;
}

int kernel_set_vmem_protection(pid_t pid, intptr_t address, size_t length,
                               int protection) {
  assert(pid == PID && address == MAP_ADDRESS && length == 0x4000 &&
         protection == (PROT_READ | PROT_EXEC));
  ++protected;
  return 0;
}

void pbs_inject_set_worker_hooks(pbs_worker_call_hook call,
                                 pbs_worker_read_hook read,
                                 pbs_worker_write_hook write) {
  call_hook = call;
  read_hook = read;
  write_hook = write;
}

int pbs_worker_link_open(pbs_worker_link *link, unsigned port) {
  assert(!port);
  link->port = 40000;
  link->cookie = 42;
  return 0;
}
int pbs_worker_link_accept(pbs_worker_link *link, unsigned timeout) {
  (void)link;
  assert(timeout == 5000 && detached == 1);
  return 0;
}
int pbs_worker_link_scratch(pbs_worker_link *link, uint64_t *address) {
  (void)link;
  *address = PRIVATE_SCRATCH;
  return 0;
}
int pbs_worker_link_call(pbs_worker_link *link, uint64_t fn,
                         const uint64_t args[6], int64_t *result,
                         unsigned timeout) {
  (void)link;
  (void)fn;
  (void)args;
  assert(timeout == 4000);
  *result = 7;
  return 0;
}
int pbs_worker_link_copy(pbs_worker_link *link, uint64_t address, void *output,
                         uint32_t length, unsigned timeout) {
  (void)link; (void)address; (void)output; (void)length; (void)timeout;
  return 0;
}
int pbs_worker_link_write(pbs_worker_link *link, uint64_t address,
                          const void *input, uint32_t length,
                          unsigned timeout) {
  (void)link; (void)address; (void)input; (void)length; (void)timeout;
  return 0;
}
int pbs_worker_link_exit(pbs_worker_link *link) {
  (void)link;
  ++exited;
  return scenario == SCENARIO_EXIT_FAILURE ? -1 : 0;
}
void pbs_worker_link_close(pbs_worker_link *link) { (void)link; }

static pbs_inject_t context(void) {
  return (pbs_inject_t){.pid = PID, .scratch = STACK_SCRATCH,
                        .scratch_len = 0x1000, .attached = 1};
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "create-timeout")) scenario = SCENARIO_CREATE_TIMEOUT;
  else if (argc > 1 && !strcmp(argv[1], "handle-read")) scenario = SCENARIO_HANDLE_READ;
  else if (argc > 1 && !strcmp(argv[1], "exit-failure")) scenario = SCENARIO_EXIT_FAILURE;
  pbs_inject_t ctx = context();
  FILE *log = tmpfile();
  assert(log);
  if (scenario == SCENARIO_CREATE_TIMEOUT) {
    assert(pbs_ps5_worker_boot(&ctx, log) && ctx.attached);
    pbs_inject_end(&ctx);
    assert(!unmapped && !joined && pbs_ps5_worker_shutdown(log));
    ctx = context();
    assert(pbs_ps5_worker_boot(&ctx, log) && ctx.attached);
    fclose(log);
    puts("PS5 worker: uncertain pthread_create quarantined without unmap or reboot");
    return 0;
  }
  if (scenario == SCENARIO_HANDLE_READ) {
    assert(pbs_ps5_worker_boot(&ctx, log) && !ctx.attached);
    assert(!unmapped && !joined && pbs_ps5_worker_shutdown(log));
    ctx = context();
    assert(pbs_ps5_worker_boot(&ctx, log) && ctx.attached);
    fclose(log);
    puts("PS5 worker: unreadable created-thread handle quarantined without double detach");
    return 0;
  }
  assert(pbs_ps5_worker_boot(&ctx, log) == 0);
  assert(!ctx.attached && ctx.worker && ctx.scratch == PRIVATE_SCRATCH);
  assert(protected == 1 && detached == 1 && call_hook && read_hook && write_hook);
  assert(call_hook(0x123, (uint64_t[6]){0}) == 7);
  if (scenario == SCENARIO_EXIT_FAILURE) {
    assert(pbs_ps5_worker_shutdown(log));
    assert(exited == 1 && !joined && !unmapped && !call_hook && !read_hook && !write_hook);
    assert(pbs_ps5_worker_shutdown(log) && exited == 1);
    ctx = context();
    assert(pbs_ps5_worker_boot(&ctx, log) && ctx.attached);
    fclose(log);
    puts("PS5 worker: uncertain RPC exit quarantined without join, unmap or reboot");
    return 0;
  }
  assert(pbs_ps5_worker_shutdown(log) == 0);
  assert(exited == 1 && attached == 1 && detached == 2 && joined == 1 &&
         unmapped == 1 && !call_hook && !read_hook && !write_hook);
  assert(pbs_ps5_worker_shutdown(log) == 0 && exited == 1);
  fclose(log);
  puts("PS5 worker: dynamic RW mapping, RX transition, detach, RPC and joined cleanup verified");
  return 0;
}
