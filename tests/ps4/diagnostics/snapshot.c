// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/diagnostics/snapshot.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
static int mode, attaches, detaches, reads, waits, resumes, epochs, sleeps;
enum {OK, ATTACH_FAIL, WAIT_FAIL, TIMEOUT, READ_FAIL, SHORT_READ, DETACH_FAIL, REPLACED};
uint64_t pbs_process_birth_epoch(pid_t pid) {
  assert(pid == 47);
  return mode == REPLACED && epochs++ ? 42 : 41;
}
int ptrace(int request, pid_t pid, caddr_t address, int data) {
  assert(pid == 47 && !data);
  if (request == PT_ATTACH) { ++attaches; assert(!address); return mode == ATTACH_FAIL ? -1 : 0; }
  if (request == PT_DETACH) { ++detaches; assert(!address); return mode == DETACH_FAIL ? -1 : 0; }
  assert(request == PT_IO); /* No register or target-memory writes allowed. */
  struct ptrace_io_desc *io = (void *)address;
  assert(io->piod_op == PIOD_READ_D && io->piod_offs == (void *)0x1000);
  ++reads;
  memset(io->piod_addr, 0x5a, io->piod_len);
  if (mode == SHORT_READ) --io->piod_len;
  return mode == READ_FAIL ? -1 : 0;
}
pid_t waitpid(pid_t pid, int *status, int options) {
  assert(pid == 47 && options == (WNOHANG | WUNTRACED)); ++waits;
  if (mode == WAIT_FAIL) { errno = ECHILD; return -1; }
  if (mode == TIMEOUT) return 0;
  *status = (SIGSTOP << 8) | 0x7f; return pid;
}
int usleep(useconds_t delay) { assert(delay == 5000); ++sleeps; return 0; }
int kill(pid_t pid, int signal) { assert(pid == 47 && signal == SIGCONT); ++resumes; return 0; }
int main(void) {
  unsigned char out[4] = {0};
  pbs_ps4_snapshot_region region = {0x1000, out, sizeof out};
  assert(pbs_ps4_read_snapshot(0, 41, &region, 1) < 0);
  assert(pbs_ps4_read_snapshot(47, 0, &region, 1) < 0);
  assert(pbs_ps4_read_snapshot(47, 41, NULL, 1) < 0);
  assert(pbs_ps4_read_snapshot(47, 41, &region, 9) < 0);
  pbs_ps4_snapshot_region invalid = {INTPTR_MAX, out, sizeof out};
  assert(pbs_ps4_read_snapshot(47, 41, &invalid, 1) < 0);
  assert(!attaches);
  for (mode = OK; mode <= REPLACED; ++mode) {
    attaches = detaches = reads = waits = resumes = epochs = sleeps = 0;
    int result = pbs_ps4_read_snapshot(47, 41, &region, 1);
    assert(attaches == 1 && detaches == (mode == ATTACH_FAIL ? 0 : 1));
    assert(result == (mode == OK ? 0 : mode == DETACH_FAIL ? -2 : -1));
    assert(resumes == (mode == DETACH_FAIL));
    if (mode == TIMEOUT) assert(waits == 50 && sleeps == 50 && !reads);
    if (mode == REPLACED) assert(!reads);
    if (mode == OK) assert(reads == 1 && out[0] == 0x5a);
  }
  puts("Read-only snapshot: bounded stop/read/detach, identity, short reads and failure resume verified");
}
