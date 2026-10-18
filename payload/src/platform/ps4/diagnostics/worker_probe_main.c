// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/diagnostics/worker_probe.h"
#include <sys/stat.h>

int main(void) {
  mkdir("/data/pbs", 0777);
  mkdir("/data/pbs/ps4", 0777);
  FILE *log = fopen("/data/pbs/ps4/worker-probe.txt", "w");
  if (!log) return 1;
  setvbuf(log, NULL, _IONBF, 0);
  fprintf(log, "PBS PS4 transport-only worker probe\n");
  pbs_inject_t ctx = {0};
  int rc = pbs_ps4_worker_probe(&ctx, log);
  fprintf(log, "worker probe exit=%d\n", rc);
  fclose(log);
  return rc ? 1 : 0;
}
