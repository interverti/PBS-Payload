// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/ps5/offset/profile.h"
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

int pbs_ps5_native_request_matches(
    const unsigned char *request, size_t length,
    const pbs_ps5_offset_profile *profile);
int pbs_ps5_native_request_matches_expected(
    const unsigned char *request, size_t length,
    const pbs_ps5_offset_profile *profile,
    uint32_t width, uint32_t height, uint32_t feature,
    uint32_t usage, uint32_t resolution, uint32_t fps_mode);

/* Compare a caller-built request with a stable live template without copying
 * any target-owned pointer or state back into the request. */
void pbs_ps5_dump_native_request_diff(
    FILE *report, const unsigned char *built, const unsigned char *native,
    size_t length, const pbs_ps5_offset_profile *profile);

#ifndef PBS_TEST
int pbs_ps5_find_native_request(
    pid_t pid, const pbs_ps5_offset_profile *profile,
    uint32_t width, uint32_t height, uint32_t feature,
    uint32_t usage, uint32_t resolution, uint32_t fps_mode,
    unsigned char *request, size_t capacity, FILE *report);
#endif
