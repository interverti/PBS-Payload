// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include "common/process/inject.h"

/* Read-only diagnostic data. Export RVAs are measured at runtime; request
 * byte positions are the caller's ABI, not discovered firmware fields. */
void pbs_ps5_dump_export_offsets(FILE *report, pid_t pid);
/* Inventory every external export used by the PS5 capture path. A short
 * read-only code fingerprint helps compare builds without assuming RVAs. */
void pbs_ps5_dump_dependencies(FILE *report, pbs_inject_t *ctx);
void pbs_ps5_dump_initial_user(FILE *report, pbs_inject_t *ctx);
void pbs_ps5_dump_request(FILE *report, const char *phase,
                          const unsigned char *request, size_t length);
void pbs_ps5_dump_request_changes(FILE *report, const unsigned char *before,
                                  const unsigned char *after, size_t length);
