// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps4_daemon_dump.h"
#include "inject.h"
#include <ps4/kernel.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

int pbs_ps4_daemon_dump(FILE *log, const pbs_ps4_firmware *fw, const char *path) {
  if (!log || !fw || !path) return -1;
  pid_t pid = pbs_find_pid("SceAvCapture");
  uint64_t epoch = pid > 0 ? pbs_process_birth_epoch(pid) : 0;
  dynlib_obj_t module = {0};
  if (!epoch || kernel_dynlib_obj(pid, 0, &module) || !module.mapbase ||
      module.mapsize > INTPTR_MAX || module.mapbase > INTPTR_MAX - module.mapsize ||
      pbs_process_birth_epoch(pid) != epoch) return -1;
  if (ptrace(PT_ATTACH, pid, NULL, 0)) return -1;
  size_t done = 0, chunks = 0, skipped = 0;
  unsigned chunk = 0x1000;
  int result = -1;
  /* One bounded attempt: no attach retry and no LWP/register operations. */
  for (unsigned elapsed = 0; elapsed < 250; elapsed += 5) {
    int status = 0;
    pid_t stopped = waitpid(pid, &status, WNOHANG | WUNTRACED);
    if (stopped == pid) {
      if (!WIFSTOPPED(status) || pbs_process_birth_epoch(pid) != epoch) break;
      static unsigned char buffer[0x1000];
      FILE *out = fopen(path, "wb");
      if (!out) break;
      result = 0;
      int probed = 0;
      while (done < (size_t)module.mapsize) {
        size_t want = (size_t)module.mapsize - done < chunk
                        ? (size_t)module.mapsize - done : chunk;
        struct ptrace_io_desc io = {
          .piod_op = PIOD_READ_D, .piod_offs = (void *)(module.mapbase + (intptr_t)done),
          .piod_addr = buffer, .piod_len = want
        };
        errno = 0;
        int window_rc = ptrace(PT_IO, pid, (caddr_t)&io, 0);
        int window_errno = errno;
        if (!window_rc && io.piod_len && io.piod_len <= want) {
          if (fwrite(buffer, 1, io.piod_len, out) != io.piod_len) { result = -1; break; }
          done += io.piod_len; ++chunks; continue;
        }
        if (!probed) {
          /* One-time comparison probe at the failed window: the latch
           * exchanges read the same image with 4-byte PT_IO and succeed. */
          uint32_t word = 0;
          struct ptrace_io_desc io4 = {
            .piod_op = PIOD_READ_D,
            .piod_offs = (void *)(module.mapbase + (intptr_t)done),
            .piod_addr = &word, .piod_len = 4
          };
          errno = 0;
          int rc4 = ptrace(PT_IO, pid, (caddr_t)&io4, 0);
          int errno4 = errno;
          fprintf(log, "Daemon dump probe: offs=%zx want=%zx rc=%d errno=%d rc4=%d errno4=%d value=%08x\n",
                  (size_t)(module.mapbase + (intptr_t)done), want, window_rc, window_errno,
                  rc4, errno4, !rc4 && io4.piod_len == 4 ? word : 0);
          probed = 1;
        }
        if (chunk > 16) { chunk >>= 2; continue; }
        /* Unreadable at the smallest window: zero the rest of the page and
         * resume at the next 4 KiB boundary instead of crawling in 16-byte
         * steps across the whole unreadable region. */
        size_t span = 0x1000 - done % 0x1000;
        if (span > (size_t)module.mapsize - done) span = (size_t)module.mapsize - done;
        memset(buffer, 0, span);
        if (fwrite(buffer, 1, span, out) != span) { result = -1; break; }
        done += span; ++chunks; ++skipped;
      }
      if (fclose(out)) result = -1;
      break;
    }
    if (stopped < 0 && errno != EINTR) break;
    usleep(5000);
  }
  if (ptrace(PT_DETACH, pid, NULL, 0)) {
    kill(pid, SIGCONT);
    if (log) fprintf(log, "Daemon dump failed: path=%s pid=%d rc=-2\n", path, (int)pid);
    return -2;
  }
  if (log) {
    if (result)
      fprintf(log, "Daemon dump failed: path=%s pid=%d done=%zu rc=%d\n",
              path, (int)pid, done, result);
    else
      fprintf(log, "Daemon dump: path=%s pid=%d base=%zx size=%zx chunk=%x chunks=%zu skipped=%zu\n",
              path, (int)pid, (size_t)module.mapbase, (size_t)module.mapsize, chunk, chunks, skipped);
  }
  return result;
}
