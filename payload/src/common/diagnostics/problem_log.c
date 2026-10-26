// SPDX-License-Identifier: GPL-3.0-or-later
/* Compact problem index implementation. See problem_log.h for the contract.
 * Deliberately dependency-free libc plus write(2): the same object links into
 * the PS4 payload, the PS5 payload and host regression tests. */
#include "common/diagnostics/problem_log.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PBS_PROBLEM_LINE_MAX 512

typedef struct {
  unsigned code;
  const char *name;
  const char *hint;
} pbs_problem_code_entry;

/* Curated from RE evidence in docs/: the meanings are what the recorded runs
 * and decompiled branches established, not guesses at undocumented semantics.
 * Keep tools/diagnose_ps4_log.py's fallback table in sync (its unit test pins
 * both). */
static const pbs_problem_code_entry pbs_problem_codes[] = {
  {0x80020003u, "KernelEsrch",
   "ESRCH: target process or thread is gone (for example a replaced RemotePlay process)"},
  {0x8002003cu, "KernelTimedOut", "ETIMEDOUT: a kernel wait budget expired"},
  {0x80a70001u, "AvcapGeneric",
   "generic SceAvCapture failure; native code maps other kernel errors and 0x8002003c here"},
  {0x80a70002u, "AvcapEmptyRing",
   "empty producer ring / timeout class; expected from flags=1 polling, not a failure by itself"},
  {0x80a70003u, "AvcapBudgetGate", "budget gate refusal; the latch/budget gate rejected the request"},
  {0x80a70004u, "AvcapRingOverrun",
   "producer overwrote unread frames; the reader resynchronized, request a fresh IDR"},
  {0x80a70008u, "AvcapUnavailable",
   "temporarily unavailable: service not ready or encoder slot busy; native RemotePlay retries "
   "16ms for a 2016ms budget; read-only slot snapshot discriminates occupancy"},
  {0x80a70009u, "AvcapCapacity", "capacity/FPS admission condition, a distinct cause from 0x80a70008"},
  {0x80a7000au, "AvcapProfileRejected",
   "profile/capability rejection; the daemon maps capabilities & 0x3f0a60 to this code"},
  {0x80bf0001u, "VencStartSequencePending",
   "encoder core +0xb0a bit set: StartSequence did not complete (state 3)"},
  {0x80bf000fu, "VencNotStarted", "encoder not started; StartSequence never ran"},
  {0x80bf0011u, "VencQueueState", "encoder dequeue after Close+Open; queue/generation state mismatch"},
};

static int pbs_problem_fd = -1;
static uint64_t pbs_problem_start_ms;
static unsigned pbs_problem_sequence;
static unsigned pbs_problem_total;
static unsigned pbs_problem_first_number;
static int pbs_problem_first_code;
static pbs_problem_severity pbs_problem_first_severity;
static pbs_problem_severity pbs_problem_worst;
static char pbs_problem_first_stage[48];

static uint64_t pbs_problem_now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

const char *pbs_problem_code_name(int code) {
  if (!code) return NULL;
  for (unsigned i = 0; i < sizeof pbs_problem_codes / sizeof pbs_problem_codes[0]; ++i)
    if (pbs_problem_codes[i].code == (unsigned)code) return pbs_problem_codes[i].name;
  if (((unsigned)code & 0xffff0000u) == 0x80020000u) return "KernelErrno";
  return NULL;
}

const char *pbs_problem_code_hint(int code) {
  if (!code) return NULL;
  for (unsigned i = 0; i < sizeof pbs_problem_codes / sizeof pbs_problem_codes[0]; ++i)
    if (pbs_problem_codes[i].code == (unsigned)code) return pbs_problem_codes[i].hint;
  if (((unsigned)code & 0xffff0000u) == 0x80020000u)
    return "SCE kernel errno class; decode the low 16 bits as an errno";
  return NULL;
}

const char *pbs_problem_severity_name(pbs_problem_severity severity) {
  switch (severity) {
    case PBS_PROBLEM_NOTE: return "note";
    case PBS_PROBLEM_WARN: return "warn";
    case PBS_PROBLEM_FAIL: return "fail";
    case PBS_PROBLEM_FATAL: return "fatal";
  }
  return "unknown";
}

static void pbs_problem_write_all(int fd, const char *line, unsigned length) {
  while (length) {
    ssize_t written = write(fd, line, length);
    if (written <= 0) {
      if (written < 0 && errno == EINTR) continue;
      return;
    }
    line += written;
    length -= (unsigned)written;
  }
}

void pbs_problem_raw(const char *line, unsigned length) {
  if (pbs_problem_fd >= 0) pbs_problem_write_all(pbs_problem_fd, line, length);
}

int pbs_problem_open(const char *path, const char *context) {
  if (pbs_problem_fd >= 0) { close(pbs_problem_fd); pbs_problem_fd = -1; }
  pbs_problem_sequence = 0;
  pbs_problem_total = 0;
  pbs_problem_first_number = 0;
  pbs_problem_first_code = 0;
  pbs_problem_first_severity = PBS_PROBLEM_NOTE;
  pbs_problem_worst = PBS_PROBLEM_NOTE;
  pbs_problem_first_stage[0] = '\0';
  pbs_problem_start_ms = pbs_problem_now_ms();
  pbs_problem_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (pbs_problem_fd < 0) return -1;
  char header[PBS_PROBLEM_LINE_MAX];
  int length = snprintf(header, sizeof header, "PBS PROBLEMS v1 t=0 %s\n",
                        context && *context ? context : "context=unavailable");
  if (length > 0) pbs_problem_write_all(pbs_problem_fd, header, (unsigned)length);
  return 0;
}

void pbs_problem_record(FILE *log, pbs_problem_severity severity, const char *stage,
                        int code, int error, const char *detail, ...) {
  char text[192];
  text[0] = '\0';
  if (detail && *detail) {
    va_list arguments;
    va_start(arguments, detail);
    vsnprintf(text, sizeof text, detail, arguments);
    va_end(arguments);
  }
  unsigned number = ++pbs_problem_sequence;
  pbs_problem_total = number;
  if (number == 1) {
    pbs_problem_first_number = number;
    pbs_problem_first_code = code;
    pbs_problem_first_severity = severity;
    if (stage) {
      strncpy(pbs_problem_first_stage, stage, sizeof pbs_problem_first_stage - 1);
      pbs_problem_first_stage[sizeof pbs_problem_first_stage - 1] = '\0';
    }
    pbs_problem_worst = severity;
  } else if (severity > pbs_problem_worst) {
    pbs_problem_worst = severity;
  }
  char line[PBS_PROBLEM_LINE_MAX];
  int length = snprintf(line, sizeof line,
      "PROBLEM n=%u t=%llu sev=%s stage=%s code=%08x name=%s errno=%d%s%s\n",
      number, (unsigned long long)(pbs_problem_now_ms() - pbs_problem_start_ms),
      pbs_problem_severity_name(severity), stage ? stage : "-", (unsigned)code,
      pbs_problem_code_name(code) ? pbs_problem_code_name(code) : "-",
      error, text[0] ? " detail=" : "", text);
  if (length > 0) {
    if (length >= (int)sizeof line) length = (int)sizeof line - 1;
    if (log) fwrite(line, 1, (size_t)length, log);
    if (pbs_problem_fd >= 0) pbs_problem_write_all(pbs_problem_fd, line, (unsigned)length);
  }
}

void pbs_problem_finish(FILE *log, int exit_status) {
  char line[PBS_PROBLEM_LINE_MAX];
  int length = snprintf(line, sizeof line,
      "SUMMARY problems=%u first=n=%u stage=%s code=%08x sev=%s worst=%s exit=%d\n",
      pbs_problem_total, pbs_problem_first_number,
      pbs_problem_first_stage[0] ? pbs_problem_first_stage : "-",
      (unsigned)pbs_problem_first_code,
      pbs_problem_total ? pbs_problem_severity_name(pbs_problem_first_severity) : "-",
      pbs_problem_total ? pbs_problem_severity_name(pbs_problem_worst) : "-",
      exit_status);
  if (length > 0) {
    if (length >= (int)sizeof line) length = (int)sizeof line - 1;
    if (log) fwrite(line, 1, (size_t)length, log);
    if (pbs_problem_fd >= 0) {
      pbs_problem_write_all(pbs_problem_fd, line, (unsigned)length);
      close(pbs_problem_fd);
      pbs_problem_fd = -1;
    }
  } else if (pbs_problem_fd >= 0) {
    close(pbs_problem_fd);
    pbs_problem_fd = -1;
  }
}
