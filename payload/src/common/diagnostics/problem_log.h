// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdio.h>

/* Compact problem index for fast failure diagnosis.
 *
 * Every problem the payload detects is recorded twice: once as a structured
 * PROBLEM line in the full capture report (with all surrounding context) and
 * once in a tiny dedicated file that survives even when the full report is
 * truncated or hard to fetch over FTP. The index starts with a context header
 * and ends with one SUMMARY line naming the *first* problem, which is usually
 * the root cause while later failures are cascades.
 *
 * Records are written with write(2) on a dedicated descriptor, so each line is
 * on disk immediately and crash handlers may append through pbs_problem_raw().
 * Emission is single-threaded by design: record problems from the payload's
 * capture thread only. */

typedef enum {
  PBS_PROBLEM_NOTE = 0,  /* unusual but recovered; keep as evidence */
  PBS_PROBLEM_WARN = 1,  /* degraded result that needs review */
  PBS_PROBLEM_FAIL = 2,  /* an operation returned failure */
  PBS_PROBLEM_FATAL = 3  /* session aborted, quarantined or unsafe to continue */
} pbs_problem_severity;

/* Symbolic name for a known SCE service code, NULL when unknown. */
const char *pbs_problem_code_name(int code);
/* One-line operator hint for a known SCE service code, NULL when unknown. */
const char *pbs_problem_code_hint(int code);
const char *pbs_problem_severity_name(pbs_problem_severity severity);

/* Truncate/create the compact index and stamp its context header. Returns 0 on
 * success; recording still mirrors into log when the index cannot be opened. */
int pbs_problem_open(const char *path, const char *context);

/* Record one problem. Mirrors a PROBLEM line into log (when not NULL) and into
 * the compact index. code/error are omitted from the line when zero, detail is
 * optional printf text truncated to keep every record on a single line. */
void pbs_problem_record(FILE *log, pbs_problem_severity severity, const char *stage,
                        int code, int error, const char *detail, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 6, 7)))
#endif
    ;

/* Append one SUMMARY line with problem count, first problem and worst
 * severity. Call once right before the capture report is closed. */
void pbs_problem_finish(FILE *log, int exit_status);

/* Async-signal-safe append of one already formatted line (no newline added).
 * Used by crash handlers; never allocates and never touches the C library
 * stdio machinery. */
void pbs_problem_raw(const char *line, unsigned length);
