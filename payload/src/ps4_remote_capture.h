// SPDX-License-Identifier: GPL-3.0-or-later
/* PS4 Avcap calls executed inside the authorized SceRemotePlay process.
 * The local libSceAvcap client is admitted as a different client class and
 * native FW11 returns 0x80a70008 before assigning an encoder. The native
 * RemotePlay path performs the same calls in its own process after its
 * ShellCore pre-notification. This backend reproduces that call context and
 * keeps the PBS HTTP/RTP transport in the payload process. */
#pragma once
#include <stddef.h>
#include <stdio.h>

int pbs_ps4_remote_capture_begin(FILE *log);
int pbs_ps4_remote_capture_init(void);
int pbs_ps4_remote_capture_active(void);
int pbs_ps4_remote_capture_end(void);
/* Release only lifecycle state acquired by PBS, before the foreign-session
 * guard reads global RemotePlay status. Keep the tracing context attached. */
int pbs_ps4_remote_prepare_release(void);

int pbs_ps4_remote_open(void **handle, const void *params);
int pbs_ps4_remote_start(void *handle);
int pbs_ps4_remote_stop(void *handle);
int pbs_ps4_remote_close(void *handle);
int pbs_ps4_remote_info(void *handle, unsigned type, void *out, size_t size);
int pbs_ps4_remote_static_info(unsigned type, void *out, size_t size);
int pbs_ps4_remote_read(void *handle, void *out, size_t size, void *info, int flags);
int pbs_ps4_remote_keyframe(void *handle);
int pbs_ps4_remote_status(unsigned char *status);
