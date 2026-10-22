// SPDX-License-Identifier: GPL-3.0-or-later
/* Read-only PS5 module dump payload. No injection or capture calls. */
#include "platform/ps5/module_dump.h"
#include <ps5/kernel.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

#define PBS_DUMP_DIR "/data/pbs"

typedef struct {
  char reserved[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void notify(const char *message) {
  notify_request_t request = {0};
  snprintf(request.message, sizeof request.message, "%s", message);
  sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
}

static pid_t find_pid(const char *name) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) || !size || size > 4u * 1024u * 1024u)
    return -1;
  unsigned char *list = malloc(size);
  if (!list) return -1;
  if (sysctl(mib, 4, list, &size, NULL, 0)) {
    free(list);
    return -1;
  }
  pid_t result = -1;
  for (unsigned char *cursor = list; cursor < list + size;) {
    size_t remaining = (size_t)(list + size - cursor);
    int record_size = 0;
    if (remaining < sizeof record_size) break;
    memcpy(&record_size, cursor, sizeof record_size);
    if (record_size < (int)(offsetof(struct kinfo_proc, ki_comm) + sizeof ((struct kinfo_proc *)0)->ki_comm) ||
        (size_t)record_size > remaining) break;
    struct kinfo_proc *process = (struct kinfo_proc *)cursor;
    if (!strcmp(process->ki_comm, name)) {
      result = process->ki_pid;
      break;
    }
    cursor += record_size;
  }
  free(list);
  return result;
}

int main(void) {
  if (mkdir(PBS_DUMP_DIR, 0777) && errno != EEXIST) {
    notify("PBS dump: cannot create /data/pbs");
    return 1;
  }
  FILE *report = fopen(PBS_DUMP_DIR "/dump.txt", "w");
  if (!report) {
    notify("PBS dump: cannot create dump.txt");
    return 1;
  }
  setvbuf(report, NULL, _IONBF, 0);
  fprintf(report, "PBS-DUMP-ONLY v1 firmware=0x%08x\n", kernel_get_fw_version());
  pid_t pid = find_pid("SceRemotePlay");
  if (pid <= 0) {
    fprintf(report, "SceRemotePlay unavailable errno=%d\n", errno);
    fclose(report);
    notify("PBS dump: SceRemotePlay unavailable; see dump.txt");
    return 1;
  }
  fprintf(report, "SceRemotePlay pid=%d\n", (int)pid);
  pbs_ps5_dump_module_files(report, pid, PBS_DUMP_DIR "/dumps");
  pid = find_pid("SceAvCapture");
  if (pid > 0) {
    fprintf(report, "SceAvCapture pid=%d\n", (int)pid);
    pbs_ps5_dump_module_files(report, pid, PBS_DUMP_DIR "/dumps");
  } else {
    fprintf(report, "SceAvCapture unavailable\n");
  }
  fprintf(report, "PBS-DUMP-ONLY finished; verify saved= entries above\n");
  fclose(report);
  notify("PBS dump: finished; check /data/pbs/dump.txt for saved files");
  return 0;
}
