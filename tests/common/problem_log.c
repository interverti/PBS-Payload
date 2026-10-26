// SPDX-License-Identifier: GPL-3.0-or-later
/* Host regression for the compact problem index. No console, no network. */
#include "common/diagnostics/problem_log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  assert(f);
  static char buffer[8192];
  size_t length = fread(buffer, 1, sizeof buffer - 1, f);
  buffer[length] = '\0';
  fclose(f);
  return buffer;
}

static void verify_code_dictionary(void) {
  assert(!strcmp(pbs_problem_code_name((int)0x80a70008u), "AvcapUnavailable"));
  assert(strstr(pbs_problem_code_hint((int)0x80a70008u), "2016ms"));
  assert(!strcmp(pbs_problem_code_name((int)0x80a70004u), "AvcapRingOverrun"));
  assert(!strcmp(pbs_problem_code_name((int)0x80a7000au), "AvcapProfileRejected"));
  assert(!strcmp(pbs_problem_code_name((int)0x80bf0001u), "VencStartSequencePending"));
  assert(!strcmp(pbs_problem_code_name((int)0x80bf0011u), "VencQueueState"));
  assert(!strcmp(pbs_problem_code_name((int)0x80020003u), "KernelEsrch"));
  assert(!strcmp(pbs_problem_code_name((int)0x8002003cu), "KernelTimedOut"));
  /* Every curated code must carry an operator hint: the index exists to be
   * actionable without opening the RE notes. */
  const unsigned codes[] = {0x80020003u, 0x8002003cu, 0x80a70001u, 0x80a70002u,
                            0x80a70003u, 0x80a70004u, 0x80a70008u, 0x80a70009u,
                            0x80a7000au, 0x80bf0001u, 0x80bf000fu, 0x80bf0011u};
  for (unsigned i = 0; i < sizeof codes / sizeof codes[0]; ++i) {
    assert(pbs_problem_code_name((int)codes[i]));
    assert(pbs_problem_code_hint((int)codes[i]));
  }
  /* Unknown codes stay unnamed, kernel errno class is decoded generically. */
  assert(!pbs_problem_code_name((int)0x80ab1234u));
  assert(!pbs_problem_code_hint((int)0x80ab1234u));
  assert(!strcmp(pbs_problem_code_name((int)0x80020063u), "KernelErrno"));
  assert(strstr(pbs_problem_code_hint((int)0x80020063u), "errno"));
  assert(!pbs_problem_code_name(0));
  assert(!strcmp(pbs_problem_severity_name(PBS_PROBLEM_NOTE), "note"));
  assert(!strcmp(pbs_problem_severity_name(PBS_PROBLEM_FATAL), "fatal"));
}

static void verify_records(void) {
  char path[] = "/tmp/pbs-problem-log-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  assert(pbs_problem_open(path, "fw=09000005 usage=0") == 0);

  FILE *mirror = tmpfile();
  assert(mirror);
  pbs_problem_record(mirror, PBS_PROBLEM_FATAL, "open", (int)0x80a70008u, 0,
                     "OpenVideoEx profile=%u failed", 19u);
  pbs_problem_record(mirror, PBS_PROBLEM_NOTE, "open", (int)0x80a70008u, 0,
                     "transient 80a70008: %u bounded retries", 4u);
  pbs_problem_record(mirror, PBS_PROBLEM_WARN, "stream", 0, 0, "low output rate");
  char crash[] = "CRASH sig=0b rip=0000000000401234 address=0000000000000000\n";
  pbs_problem_raw(crash, sizeof crash - 1);
  pbs_problem_finish(mirror, 1);

  char *index = read_file(path);
  assert(strstr(index, "PBS PROBLEMS v1 t=0 fw=09000005 usage=0\n"));
  const char *first = strstr(index, "PROBLEM n=1 ");
  assert(first);
  assert(strstr(first, "sev=fatal stage=open code=80a70008 name=AvcapUnavailable errno=0"
                       " detail=OpenVideoEx profile=19 failed"));
  const char *second = strstr(index, "PROBLEM n=2 ");
  assert(second);
  assert(strstr(second, "sev=note stage=open code=80a70008 name=AvcapUnavailable"));
  const char *third = strstr(index, "PROBLEM n=3 ");
  assert(third);
  assert(strstr(third, "sev=warn stage=stream code=00000000 name=- errno=0 detail=low output rate"));
  /* The crash handler line is appended verbatim and stays retrievable. */
  assert(strstr(index, "CRASH sig=0b rip=0000000000401234 address=0000000000000000"));
  /* SUMMARY is the last line and names the FIRST problem as root cause. */
  const char *summary = strstr(index, "SUMMARY problems=3 ");
  assert(summary);
  assert(strstr(summary, "first=n=1 stage=open code=80a70008 sev=fatal worst=fatal exit=1"));
  assert(strchr(summary, '\n') && !strchr(strchr(summary, '\n') + 1, '\n'));

  /* Mirror receives the same records so capture.txt stays greppable. */
  fflush(mirror);
  char mirrored[4096];
  rewind(mirror);
  size_t length = fread(mirrored, 1, sizeof mirrored - 1, mirror);
  mirrored[length] = '\0';
  fclose(mirror);
  assert(strstr(mirrored, "PROBLEM n=1 "));
  assert(strstr(mirrored, "PROBLEM n=3 "));
  assert(strstr(mirrored, "SUMMARY problems=3 "));
  assert(!strstr(mirrored, "CRASH"));

  /* A clean run reports zero problems and no fabricated root cause. */
  assert(pbs_problem_open(path, "fw=09000005") == 0);
  pbs_problem_finish(NULL, 0);
  index = read_file(path);
  assert(strstr(index, "SUMMARY problems=0 first=n=0 stage=- code=00000000 sev=- worst=- exit=0"));
  assert(!strstr(index, "PROBLEM "));
  unlink(path);
}

int main(void) {
  verify_code_dictionary();
  verify_records();
  puts("Problem log: code dictionary, record format, mirror and SUMMARY verified");
  return 0;
}
