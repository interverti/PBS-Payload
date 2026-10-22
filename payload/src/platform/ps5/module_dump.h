// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdio.h>
#include <sys/types.h>

/* Copy selected on-disk modules mapped by SceRemotePlay. No target attach or
 * module patching is performed. Failures are reported per file. */
void pbs_ps5_dump_module_files(FILE *report, pid_t pid, const char *output_root);
