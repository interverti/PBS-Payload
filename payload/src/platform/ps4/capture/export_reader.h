// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common/process/inject.h"

/* Only exported Avcap/libc calls; the handle remains opaque. The buffer is
 * owned by this reader and allocated on the running worker, never ptrace. */
typedef struct {
  intptr_t read, allocate, release, buffer;
} pbs_ps4_export_reader;

int pbs_ps4_export_reader_init(pbs_ps4_export_reader *r, pbs_inject_t *ctx);
int pbs_ps4_export_reader_read(pbs_ps4_export_reader *r, pbs_inject_t *ctx,
                             void *handle, void *out, size_t size, void *info,
                             int flags);
int pbs_ps4_export_reader_release(pbs_ps4_export_reader *r, pbs_inject_t *ctx);
