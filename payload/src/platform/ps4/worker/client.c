// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/worker/client.h"

#include <sys/select.h>
#include <string.h>
#include <time.h>

/* The worker thread stub (platform/ps4/worker/entry.S). Copied into the worker mapping
 * so no instruction is fetched from the payload's own image. */
extern const unsigned char pbs_worker_code_start[], pbs_worker_code_end[];

/* Phase values shared with platform/ps4/worker/entry.S. */
enum {
  PBS_WORKER_PHASE_IDLE = 0,
  PBS_WORKER_PHASE_CALL = 1,
  PBS_WORKER_PHASE_DONE = 2,
  PBS_WORKER_PHASE_EXIT = 3,
  PBS_WORKER_PHASE_EXITED = 4
};

static uint64_t worker_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int worker_read_u32(pbs_worker_t *w, uint64_t address, uint32_t *value) {
  return w->io->read(w->io->opaque, address, value, sizeof *value) ? -1 : 0;
}

static int worker_write_u32(pbs_worker_t *w, uint64_t address, uint32_t value) {
  return w->io->write(w->io->opaque, address, &value, sizeof value) ? -1 : 0;
}

void pbs_worker_job_init(pbs_worker_job *job, uint64_t sleep_fn,
                         uint64_t unmap_fn, uint64_t map_base,
                         uint64_t map_size) {
  memset(job, 0, sizeof *job);
  job->sleep_fn = sleep_fn;
  job->unmap_fn = unmap_fn;
  job->map_base = map_base;
  job->map_size = map_size;
}

int pbs_worker_wait_ready(pbs_worker_t *w) {
  const uint64_t began = worker_now_ms();
  const uint64_t budget = w->timeout_ms ? w->timeout_ms : 2000u;
  while (worker_now_ms() - began < budget) {
    uint32_t ready = 0;
    if (worker_read_u32(w, w->job + offsetof(pbs_worker_job, ready), &ready))
      return -1;
    if (ready) return 0;
    struct timespec pause = {0, 200000};
    nanosleep(&pause, NULL);
  }
  return -1;
}

static int worker_wait_phase(pbs_worker_t *w, uint32_t want, uint64_t budget) {
  const uint64_t began = worker_now_ms();
  while (worker_now_ms() - began < budget) {
    uint32_t phase = 0;
    if (worker_read_u32(w, w->job + offsetof(pbs_worker_job, phase), &phase))
      return -1;
    if (phase == want) return 0;
    if (phase > PBS_WORKER_PHASE_EXITED) return -1;
    struct timespec pause = {0, 50000};
    nanosleep(&pause, NULL);
  }
  return -1;
}

int pbs_worker_call(pbs_worker_t *w, uint64_t fn, const uint64_t args[6],
                    int64_t *result) {
  if (!w || !w->io || w->fatal) return -1;
  uint64_t request[7];
  request[0] = fn;
  memcpy(&request[1], args, sizeof(uint64_t) * 6);
  if (w->io->write(w->io->opaque,
                   w->job + offsetof(pbs_worker_job, function), request,
                   sizeof request))
    return -1;
  if (worker_write_u32(w, w->job + offsetof(pbs_worker_job, phase), PBS_WORKER_PHASE_CALL)) return -1;
  const uint64_t budget = w->timeout_ms ? w->timeout_ms : 2000u;
  if (worker_wait_phase(w, PBS_WORKER_PHASE_DONE, budget)) {
    /* A call that never completes leaves the worker unusable: the thread is
     * still inside fn and would overwrite the next answer. */
    w->fatal = 1;
    return -1;
  }
  int64_t value = 0;
  if (w->io->read(w->io->opaque, w->job + offsetof(pbs_worker_job, result),
                  &value, sizeof value))
    return -1;
  if (result) *result = value;
  return 0;
}

int pbs_worker_exit(pbs_worker_t *w) {
  if (!w || !w->io || !w->job) return -1;
  if (worker_write_u32(w, w->job + offsetof(pbs_worker_job, phase), PBS_WORKER_PHASE_EXIT)) return -1;
  /* The thread unmaps the whole mapping before returning, so a failed read
   * after this point is the expected outcome. */
  const uint64_t began = worker_now_ms();
  while (worker_now_ms() - began < 1000u) {
    uint32_t phase = 0;
    if (worker_read_u32(w, w->job + offsetof(pbs_worker_job, phase), &phase))
      return 0;
    if (phase >= PBS_WORKER_PHASE_EXITED) return 0;
    struct timespec pause = {0, 100000};
    nanosleep(&pause, NULL);
  }
  return -1;
}
/* ------------------------------------------------------------------ */
/* Socket transport                                                   */
/* ------------------------------------------------------------------ */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "platform/ps4/worker/remote.h"

static int link_send_full(int fd, const void *buffer, size_t size,
                          unsigned timeout_ms) {
  struct timeval tv = {(time_t)(timeout_ms / 1000u),
                       (suseconds_t)((timeout_ms % 1000u) * 1000u)};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  const unsigned char *in = buffer;
  size_t done = 0;
  while (done < size) {
    ssize_t n = send(fd, in + done, size - done, MSG_NOSIGNAL);
    if (n <= 0) return -1;
    done += (size_t)n;
  }
  return 0;
}

static int link_recv_full(int fd, void *buffer, size_t size,
                          unsigned timeout_ms) {
  struct timeval tv = {(time_t)(timeout_ms / 1000u),
                       (suseconds_t)((timeout_ms % 1000u) * 1000u)};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  unsigned char *out = buffer;
  size_t done = 0;
  while (done < size) {
    ssize_t n = recv(fd, out + done, size - done, 0);
    if (n <= 0) return -1;
    done += (size_t)n;
  }
  return 0;
}

int pbs_worker_link_open(pbs_worker_link *link, unsigned port_hint) {
  if (!link) return -1;
  memset(link, 0, sizeof *link);
  link->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (link->listen_fd < 0) return -1;
  int one = 1;
  setsockopt(link->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)port_hint);
  if (bind(link->listen_fd, (const struct sockaddr *)&addr, sizeof addr) ||
      listen(link->listen_fd, 1)) {
    close(link->listen_fd);
    link->listen_fd = -1;
    return -1;
  }
  socklen_t len = sizeof addr;
  if (getsockname(link->listen_fd, (struct sockaddr *)&addr, &len)) {
    close(link->listen_fd);
    link->listen_fd = -1;
    return -1;
  }
  link->port = ntohs(addr.sin_port);
  link->fd = -1;
  /* Cookie binds this session to the worker hello and rejects stale sockets. */
  link->cookie = ((uint32_t)link->port << 16) ^ 0x50425357u;
  return 0;
}

int pbs_worker_link_accept(pbs_worker_link *link, unsigned timeout_ms) {
  if (!link || link->listen_fd < 0) return -1;
  const uint64_t began = worker_now_ms();
  for (;;) {
    /* SO_RCVTIMEO does not bound accept(): wait for readiness first, or the
     * payload would hang here forever after the target detached. */
    fd_set set;
    FD_ZERO(&set);
    FD_SET(link->listen_fd, &set);
    struct timeval slice = {0, 100000};
    int ready = select(link->listen_fd + 1, &set, NULL, NULL, &slice);
    if (ready <= 0) {
      if (worker_now_ms() - began >= timeout_ms) return -1;
      continue;
    }
    struct sockaddr_in peer;
    socklen_t len = sizeof peer;
    int fd = accept(link->listen_fd, (struct sockaddr *)&peer, &len);
    if (fd >= 0) {
      int one = 1;
      if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one)) {
        close(fd);
        return -1;
      }
      uint32_t hello = 0;
      if (link_recv_full(fd, &hello, sizeof hello, 1000) ||
          hello != link->cookie) {
        close(fd);
        return -1;
      }
      link->fd = fd;
      return 0;
    }
    if (worker_now_ms() - began >= timeout_ms) return -1;
    struct timespec pause = {0, 1000000};
    nanosleep(&pause, NULL);
  }
}

int pbs_worker_link_call(pbs_worker_link *link, uint64_t fn,
                         const uint64_t args[6], int64_t *result,
                         unsigned timeout_ms) {
  if (!link || link->fd < 0 || link->fatal) return -1;
  unsigned char request[PBS_WORKER_REQUEST_SIZE];
  memset(request, 0, sizeof request);
  uint32_t cmd = PBS_WORKER_CMD_CALL;
  memcpy(request, &cmd, sizeof cmd);
  memcpy(request + 0x08, &fn, sizeof fn);
  memcpy(request + 0x10, args, sizeof(uint64_t) * 6);
  unsigned char reply[PBS_WORKER_REPLY_SIZE];
  if (link_send_full(link->fd, request, sizeof request, timeout_ms) ||
      link_recv_full(link->fd, reply, sizeof reply, timeout_ms)) {
    link->fatal = 1;
    return -1;
  }
  int64_t value = 0;
  memcpy(&value, reply, sizeof value);
  if (result) *result = value;
  return 0;
}

int pbs_worker_link_copy(pbs_worker_link *link, uint64_t address, void *dst,
                         uint32_t size, unsigned timeout_ms) {
  if (!link || link->fd < 0 || link->fatal || !dst || !size) return -1;
  if (size > PBS_WORKER_COPY_MAX) return -1;
  unsigned char request[PBS_WORKER_REQUEST_SIZE];
  memset(request, 0, sizeof request);
  uint32_t cmd = PBS_WORKER_CMD_COPY;
  memcpy(request, &cmd, sizeof cmd);
  memcpy(request + 0x08, &address, sizeof address);
  memcpy(request + 0x40, &size, sizeof size);
  unsigned char reply[PBS_WORKER_REPLY_SIZE];
  if (link_send_full(link->fd, request, sizeof request, timeout_ms) ||
      link_recv_full(link->fd, reply, sizeof reply, timeout_ms)) {
    link->fatal = 1;
    return -1;
  }
  uint32_t got = 0;
  memcpy(&got, reply + 0x08, sizeof got);
  if (got != size) return -1;
  if (link_recv_full(link->fd, dst, size, timeout_ms)) {
    link->fatal = 1;
    return -1;
  }
  return 0;
}

int pbs_worker_link_write(pbs_worker_link *link, uint64_t address,
                          const void *src, uint32_t size, unsigned timeout_ms) {
  if (!link || link->fd < 0 || link->fatal || !src || !size) return -1;
  if (size > PBS_WORKER_WRITE_MAX) return -1;
  unsigned char request[PBS_WORKER_REQUEST_SIZE];
  memset(request, 0, sizeof request);
  uint32_t cmd = PBS_WORKER_CMD_WRITE;
  memcpy(request, &cmd, sizeof cmd);
  memcpy(request + 0x08, &address, sizeof address);
  memcpy(request + 0x40, &size, sizeof size);
  unsigned char reply[PBS_WORKER_REPLY_SIZE];
  if (link_send_full(link->fd, request, sizeof request, timeout_ms) ||
      link_send_full(link->fd, src, size, timeout_ms) ||
      link_recv_full(link->fd, reply, sizeof reply, timeout_ms)) {
    link->fatal = 1;
    return -1;
  }
  uint64_t written = 0;
  memcpy(&written, reply, sizeof written);
  if (written != size) {
    link->fatal = 1;
    return -1;
  }
  return 0;
}

int pbs_worker_link_scratch(pbs_worker_link *link, uint64_t *address) {
  if (!link || link->fd < 0 || link->fatal || !address) return -1;
  unsigned char request[PBS_WORKER_REQUEST_SIZE] = {0};
  uint32_t cmd = PBS_WORKER_CMD_SCRATCH;
  memcpy(request, &cmd, sizeof cmd);
  unsigned char reply[PBS_WORKER_REPLY_SIZE];
  if (link_send_full(link->fd, request, sizeof request, 1000) ||
      link_recv_full(link->fd, reply, sizeof reply, 1000)) {
    link->fatal = 1;
    return -1;
  }
  memcpy(address, reply, sizeof *address);
  return *address ? 0 : -1;
}

int pbs_worker_link_exit(pbs_worker_link *link) {
  if (!link || link->fd < 0 || link->fatal) return -1;
  unsigned char request[PBS_WORKER_REQUEST_SIZE];
  memset(request, 0, sizeof request);
  uint32_t cmd = PBS_WORKER_CMD_EXIT;
  memcpy(request, &cmd, sizeof cmd);
  unsigned char reply[PBS_WORKER_REPLY_SIZE];
  if (link_send_full(link->fd, request, sizeof request, 1000) ||
      link_recv_full(link->fd, reply, sizeof reply, 1000))
    return -1;
  /* EXIT is complete only once the worker closes its socket. Keep cave code
   * mapped: EOF does not prove the pthread entry has returned yet. */
  unsigned char unexpected;
  return recv(link->fd, &unexpected, 1, 0) == 0 ? 0 : -1;
}

void pbs_worker_link_close(pbs_worker_link *link) {
  if (!link) return;
  if (link->fd >= 0) close(link->fd);
  if (link->listen_fd >= 0) close(link->listen_fd);
  link->fd = -1;
  link->listen_fd = -1;
}
