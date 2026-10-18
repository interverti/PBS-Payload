// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/diagnostics/snapshot.h"
#include "common/process/inject.h"
#include <errno.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

int pbs_ps4_read_snapshot(pid_t pid, uint64_t epoch,
                          const pbs_ps4_snapshot_region *regions, size_t count) {
  if (pid <= 0 || !epoch || !regions || !count || count > 8) return -1;
  for (size_t i = 0; i < count; ++i)
    if (regions[i].address <= 0 || !regions[i].destination ||
        !regions[i].size || regions[i].size > 4096 ||
        (uint64_t)regions[i].address > INTPTR_MAX - regions[i].size) return -1;
  if (pbs_process_birth_epoch(pid) != epoch) return -1;
  if (ptrace(PT_ATTACH, pid, NULL, 0)) return -1;
  int result = -1;
  /* One bounded attempt: no attach retry and no LWP/register operations. */
  for (unsigned elapsed = 0; elapsed < 250; elapsed += 5) {
    int status = 0;
    pid_t stopped = waitpid(pid, &status, WNOHANG | WUNTRACED);
    if (stopped == pid) {
      if (!WIFSTOPPED(status) || pbs_process_birth_epoch(pid) != epoch) break;
      result = 0;
      for (size_t i = 0; i < count; ++i) {
        struct ptrace_io_desc io = {
          .piod_op = PIOD_READ_D, .piod_offs = (void *)regions[i].address,
          .piod_addr = regions[i].destination, .piod_len = regions[i].size
        };
        if (ptrace(PT_IO, pid, (caddr_t)&io, 0) || io.piod_len != regions[i].size) {
          result = -1;
          break;
        }
      }
      break;
    }
    if (stopped < 0 && errno != EINTR) break;
    usleep(5000);
  }
  if (ptrace(PT_DETACH, pid, NULL, 0)) {
    kill(pid, SIGCONT);
    return -2;
  }
  return result;
}
