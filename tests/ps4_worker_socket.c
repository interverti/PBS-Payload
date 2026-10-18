/* Host prototype for the socket transport.
 *
 * The worker loop is executed from a byte copy of its own machine code, the
 * way the payload will run it inside RemotePlay, and talks to the payload-side
 * client over a real loopback TCP socket. Verifies: handshake cookie, six
 * argument calls, memory copy back to the payload, and a clean exit.
 *
 * The code page and the context page are made READ-ONLY before the thread
 * starts, matching the console code cave (r-x). This is a regression for the
 * 18/10/2026 hardware crash: the worker wrote its status into the context and
 * SceRemotePlay died with "page fault (user write data, protection violation)".
 * Any store through the context or the code now fails this test. */
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ps4_worker.h"
#include "ps4_worker_remote.h"
#include "ps4_worker_addr.h"

/* Model the target ABI at the connect boundary, then translate for Linux.
 * Using a Linux sockaddr directly previously hid a console-only failure. */
static int orbis_connect(int fd, const void *raw, unsigned size) {
  const unsigned char *addr = raw;
  assert(size == 16 && addr[0] == 16 && addr[1] == AF_INET);
  for (unsigned i = 8; i < 16; ++i) assert(addr[i] == 0);
  struct sockaddr_in native = {0};
  native.sin_family = AF_INET;
  memcpy(&native.sin_port, addr + 2, 2);
  memcpy(&native.sin_addr, addr + 4, 4);
  return connect(fd, (const struct sockaddr *)&native, sizeof native);
}

static int64_t add6(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                    uint64_t f) {
  return (int64_t)(a + b + c + d + e + f);
}

static unsigned char frame_source[0x1000];

int main(void) {
  pbs_worker_link link;
  assert(pbs_worker_link_open(&link, 0) == 0);
  assert(link.port != 0);

  /* Code page: writable while copying, then read+execute only, like the cave. */
  const size_t blob = (size_t)(__stop_pbs_worker_remote - __start_pbs_worker_remote);
  assert(blob > 0 && blob < 0x1000);
  void *code = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(code != MAP_FAILED);
  memcpy(code, __start_pbs_worker_remote, blob);
  assert(mprotect(code, 0x1000, PROT_READ | PROT_EXEC) == 0);

  /* Context page: filled by the "payload", then read-only for the worker. */
  void *ctxmem = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(ctxmem != MAP_FAILED);
  pbs_worker_remote_ctx *ctx = ctxmem;
  memset(ctx, 0, sizeof *ctx);
  ctx->fn_socket = (uint64_t)(uintptr_t)&socket;
  ctx->fn_connect = (uint64_t)(uintptr_t)&orbis_connect;
  ctx->fn_recv = (uint64_t)(uintptr_t)&recv;
  ctx->fn_send = (uint64_t)(uintptr_t)&send;
  ctx->fn_close = (uint64_t)(uintptr_t)&close;
  ctx->fn_sleep = (uint64_t)(uintptr_t)&usleep;
  ctx->fn_setsockopt = (uint64_t)(uintptr_t)&setsockopt;
  ctx->cookie = link.cookie;
  pbs_ps4_worker_addr(ctx->addr, (uint16_t)link.port);
  assert(mprotect(ctxmem, 0x1000, PROT_READ) == 0);

  pthread_t worker;
  assert(pthread_create(&worker, NULL, (void *(*)(void *))code, ctx) == 0);
  assert(pbs_worker_link_accept(&link, 3000) == 0);

  uint64_t private_scratch = 0;
  assert(pbs_worker_link_scratch(&link, &private_scratch) == 0);
  assert(private_scratch != 0);
  unsigned char private_data[0x1000], private_back[0x1000];
  memset(private_data, 0xa7, sizeof private_data);
  assert(pbs_worker_link_write(&link, private_scratch, private_data,
                              sizeof private_data, 2000) == 0);
  int64_t result = 0;
  uint64_t args[6] = {1, 2, 3, 4, 5, 6};
  assert(pbs_worker_link_call(&link, (uint64_t)(uintptr_t)&add6, args, &result,
                              2000) == 0);
  assert(result == 21);
  uint64_t args2[6] = {10, 20, 30, 40, 50, 60};
  assert(pbs_worker_link_call(&link, (uint64_t)(uintptr_t)&add6, args2, &result,
                              2000) == 0);
  assert(result == 210);
  assert(pbs_worker_link_copy(&link, private_scratch, private_back,
                             sizeof private_back, 2000) == 0);
  assert(memcmp(private_data, private_back, sizeof private_data) == 0);

  /* Copy 0x800 bytes of the payload's own memory through the worker. */
  for (size_t i = 0; i < sizeof frame_source; ++i)
    frame_source[i] = (unsigned char)(i * 7u);
  unsigned char back[0x800];
  memset(back, 0, sizeof back);
  assert(pbs_worker_link_copy(&link, (uint64_t)(uintptr_t)frame_source, back,
                              (uint32_t)sizeof back, 2000) == 0);
  assert(memcmp(back, frame_source, sizeof back) == 0);

  /* Exercise WRITE at the protocol limit, then COPY the result back. */
  unsigned char written[PBS_WORKER_WRITE_MAX];
  for (size_t i = 0; i < sizeof written; ++i)
    written[i] = (unsigned char)(255u - i);
  assert(pbs_worker_link_write(&link, (uint64_t)(uintptr_t)frame_source,
                               written, sizeof written, 2000) == 0);
  unsigned char readback[sizeof written];
  assert(pbs_worker_link_copy(&link, (uint64_t)(uintptr_t)frame_source,
                              readback, sizeof readback, 2000) == 0);
  assert(memcmp(readback, written, sizeof written) == 0);
  assert(pbs_worker_link_write(&link, (uint64_t)(uintptr_t)frame_source,
                               written, 0, 2000) == -1);
  assert(pbs_worker_link_write(&link, (uint64_t)(uintptr_t)frame_source,
                               written, sizeof written + 1, 2000) == -1);

  assert(pbs_worker_link_exit(&link) == 0);
  assert(pthread_join(worker, NULL) == 0);
  pbs_worker_link_close(&link);
  munmap(ctxmem, 0x1000);
  munmap(code, 0x1000);
  /* A peer disappearing during a job must return failure, never SIGPIPE. */
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  close(pair[1]);
  pbs_worker_link broken = {.fd = pair[0], .listen_fd = -1};
  assert(pbs_worker_link_call(&broken, 1, args, &result, 100) < 0);
  assert(broken.fatal);
  pbs_worker_link_close(&broken);
  printf("worker socket: read-only code/context, Orbis address, handshake, "
         "calls, copy, write and exit verified\n");
  return 0;
}
