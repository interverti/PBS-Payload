// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/diagnostics/daemon_dump.h"
#include "common/process/inject.h"
#include <ps4/kernel.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
#define MAP 0x400000
#define SIZE 0x2000
#define DUMP "/tmp/pbs_daemon_dump_test.bin"
static unsigned char memory[SIZE], file[SIZE];
static size_t file_len, io_calls, skipped;
static int attached, cont;
static int short_read, unreadable_high, identity_fail, identity_calls;
static int attach_fail, detach_fail;
pid_t pbs_find_pid(const char *name) { assert(!strcmp(name, "SceAvCapture")); return 43; }
uint64_t pbs_process_birth_epoch(pid_t pid) {
  assert(pid == 43);
  return identity_fail && identity_calls++ ? 42 : 41;
}
int kernel_dynlib_obj(pid_t pid, uint32_t handle, dynlib_obj_t *out) {
  assert(pid == 43 && !handle); out->mapbase = MAP; out->mapsize = SIZE; return 0;
}
int ptrace(int request, pid_t pid, caddr_t address, int data) {
  assert(pid == 43);
  if (request == PT_ATTACH) {
    if (attach_fail) { errno = EPERM; return -1; }
    assert(!attached && !data); attached = 1; return 0;
  }
  assert(attached);
  if (request == PT_DETACH) {
    if (detach_fail) { --detach_fail; return -1; }
    attached = 0; return 0;
  }
  assert(request == PT_IO && !data);
  struct ptrace_io_desc *io = (void *)address;
  uintptr_t offset = (uintptr_t)io->piod_offs - MAP;
  assert(io->piod_op == PIOD_READ_D && offset + io->piod_len <= SIZE);
  if (short_read && io_calls == 1) {
    assert(io->piod_len > 0x400);
    io->piod_len = 0x400;
  }
  if (unreadable_high && offset >= 0x1000) {
    ++skipped; io->piod_len = 0; return -1;
  }
  ++io_calls;
  memcpy(io->piod_addr, memory + offset, io->piod_len);
  return 0;
}
pid_t waitpid(pid_t pid, int *status, int options) {
  assert(pid == 43 && attached && options == (WNOHANG | WUNTRACED));
  *status = (SIGSTOP << 8) | 0x7f; return pid;
}
int usleep(useconds_t delay) { assert(delay == 5000); return 0; }
int kill(pid_t pid, int signal) { assert(pid == 43 && signal == SIGCONT); cont = 1; return 0; }
static int read_file(void) {
  FILE *f = fopen(DUMP, "rb");
  if (!f) return -1;
  file_len = fread(file, 1, sizeof file, f);
  fclose(f);
  unlink(DUMP);
  return 0;
}
int main(void) {
  FILE *log = tmpfile(); assert(log);
  for (size_t i = 0; i < SIZE; ++i) memory[i] = (unsigned char)(i * 7 + 3);
  /* Full image in two 4 KiB windows. */
  assert(!attached && !io_calls);
  assert(!pbs_ps4_daemon_dump(log, pbs_ps4_firmware_find(0x11008001), DUMP));
  assert(!attached && io_calls == 2);
  assert(!read_file() && file_len == SIZE && !memcmp(file, memory, SIZE));
  /* Kernel shortens the first read: the window resumes from the copy length. */
  short_read = 1; io_calls = 0;
  assert(!pbs_ps4_daemon_dump(log, pbs_ps4_firmware_find(0x11008001), DUMP));
  assert(io_calls == 3);
  assert(!read_file() && file_len == SIZE && !memcmp(file, memory, SIZE));
  /* Upper half unreadable at every window down to 16 bytes: zero-filled. */
  short_read = 0; unreadable_high = 1; io_calls = 0; skipped = 0;
  assert(!pbs_ps4_daemon_dump(log, pbs_ps4_firmware_find(0x11008001), DUMP));
  assert(skipped == 6);
  assert(!read_file() && file_len == SIZE);
  assert(!memcmp(file, memory, 0x1000));
  for (size_t i = 0x1000; i < SIZE; ++i) assert(!file[i]);
  /* Provision and identity failures never attach. */
  unreadable_high = 0; io_calls = 0;
  assert(pbs_ps4_daemon_dump(NULL, pbs_ps4_firmware_find(0x11008001), DUMP) == -1 && !attached);
  assert(pbs_ps4_daemon_dump(log, NULL, DUMP) == -1 && !attached);
  identity_fail = 1;
  assert(pbs_ps4_daemon_dump(log, pbs_ps4_firmware_find(0x11008001), DUMP) == -1 && !attached);
  identity_fail = 0; attach_fail = 1;
  assert(pbs_ps4_daemon_dump(log, pbs_ps4_firmware_find(0x11008001), DUMP) == -1 && !attached);
  /* Detach failure keeps the process schedulable and reports -2. */
  attach_fail = 0; detach_fail = 1;
  assert(pbs_ps4_daemon_dump(log, pbs_ps4_firmware_find(0x11008001), DUMP) == -2);
  assert(cont && attached);
  puts("Daemon image dump: full image, short-read resume, unreadable window skip and cleanup verified");
}
