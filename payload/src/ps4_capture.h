// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdio.h>

int pbs_ps4_capture_begin(FILE *log);
void pbs_ps4_capture_end(void);
int pbs_ps4_init(void);
int pbs_ps4_term(void);
int pbs_ps4_open(void **handle, const void *params);
int pbs_ps4_start(void *handle);
int pbs_ps4_stop(void *handle);
int pbs_ps4_close(void *handle);
int pbs_ps4_info(void *handle, unsigned type, void *out, size_t size);
/* Read the current system capture request without changing it. */
int pbs_ps4_static_info(unsigned type, void *out, size_t size);
/* Optional ShellCore read-only observation; -1 if not exported/available.
 * Native SceShellCore uses sceShellCoreUtilGetRemotePlayStatus(uint8_t *). */
int pbs_ps4_remoteplay_status(unsigned char *status);
/* Safe, read-only SceAvSetting 0x7000 current-output snapshot. The observed
 * value at the native out+0x10 offset is an opaque output descriptor 1..8;
 * it is NOT the VideoOut 0x7001/0x7002 mode ID. */
int pbs_ps4_output_mode_snapshot(unsigned long long *descriptor,
                                 unsigned char raw_output[0x28]);
/* Native RemotePlay reads an output-bus 40-byte snapshot with mode bit1
 * at +0x20. Does not identify capture ownership or fetch screen pixels. */
int pbs_ps4_videoout_bus_snapshot(unsigned *flags,
                                  unsigned char raw_output[0x28]);
int pbs_ps4_read(void *handle, void *out, size_t size, void *info, int flags);
int pbs_ps4_keyframe(void *handle);
