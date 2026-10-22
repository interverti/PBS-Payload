// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

int pbs_h264_server_start(int port, int width, int height, const unsigned char *sps, size_t sps_len,
                           const unsigned char *pps, size_t pps_len);
int pbs_h264_server_start_at_rate(int port, int width, int height, int fps,
    const unsigned char *sps, size_t sps_len, const unsigned char *pps, size_t pps_len);
int pbs_h264_publish(const void *data, size_t length, uint32_t pts);
int pbs_audio_publish(const void *data, size_t length, uint32_t pts);
void pbs_audio_configure(const char *codec, int channels, int rate);
int pbs_h264_server_running(void);
int pbs_h264_take_keyframe_request(void);
void pbs_h264_server_stop(void);
/* Call from the capture thread after stop, before the payload can unload. */
void pbs_h264_server_wait(void);
void pbs_h264_discontinuity(void);
int pbs_h264_take_restart_request(void);
/* Only enable on backends that can reopen all advertised capture profiles. */
void pbs_h264_enable_profiles(void);
int pbs_h264_take_profile_request(int *height);
void pbs_h264_profile_failed(void);
