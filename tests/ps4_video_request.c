// SPDX-License-Identifier: GPL-3.0-or-later
/* Native PS4 firmware 11.00 request fields reconstructed from SceRemotePlay;
 * catches ABI drift and bitrate-vs-fps confusion in +0x40/+0x44. */
#include "ps4_video_request.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  uint32_t params[PBS_PS4_VIDEO_REQUEST_WORDS];
  assert(PBS_PS4_VIDEO_REQUEST_SIZE == 264);
  assert(pbs_ps4_make_video_request(params, 720, 0, 0) == 0);
  assert(params[0] == 0 && params[1] == 0);
  assert(params[2] == 19 && params[3] == 19);
  assert(params[0x18 / 4] == 0x44 && params[0x1c / 4] == 60);
  assert(params[0x24 / 4] == 1 && params[0x30 / 4] == 9);
  assert(params[0x38 / 4] == 35 && params[0x3c / 4] == 51);
  assert(params[0x10 / 4] == 9090909 && params[0x14 / 4] == 10000000);
  assert(params[0x40 / 4] == 3749 && params[0x44 / 4] == 2657);
  assert(params[0x4c / 4] == 25);
  assert(params[0x5c / 4] == 0 && params[0x60 / 4] == 0);
  assert(params[0x64 / 4] == 0);
  for (unsigned i = 0x68; i < 0x88; ++i) assert(((unsigned char *)params)[i] == 0);
  assert(pbs_ps4_make_video_request(params, 720, 0, 1) == 0);
  assert(params[0x5c / 4] == 0x01800000 && params[0x60 / 4] == 0x01600180);
  assert(params[0x64 / 4] == 0x00034000);
  assert(!memcmp((unsigned char *)params + 0x68,
                 "/SceRemotePlayImagePaste", sizeof "/SceRemotePlayImagePaste"));
  for (unsigned i = 0x68 + sizeof "/SceRemotePlayImagePaste"; i < 0x88; ++i)
    assert(((unsigned char *)params)[i] == 0);
  for (unsigned i = 0x88; i < 0x108; ++i) assert(((unsigned char *)params)[i] == 0);
  assert(pbs_ps4_make_video_request(params, 1080, 0, 0) == 0);
  assert(params[1] == 0 && params[2] == 20 && params[3] == 20);
  assert(params[0x4c / 4] == 20 && params[0x1c / 4] == 60);
  assert(params[0x14 / 4] == 10000000);
  assert(params[0x40 / 4] == 3749 && params[0x44 / 4] == 2657);
  for (unsigned i = 0x88; i < 0x108; ++i)
    assert(((unsigned char *)params)[i] == 0);
  assert(pbs_ps4_make_video_request(params, 1080, 1, 0) == 0);
  assert(params[1] == 1 && params[2] == 20 && params[3] == 20);
  assert(params[0x4c / 4] == 20);
  assert(pbs_ps4_make_video_request(params, 1440, 0, 0) == -1);
  assert(pbs_ps4_make_video_request(params, 720, 2, 0) == -1);
  assert(pbs_ps4_make_video_request(params, 720, 0, 2) == -1);
  assert(pbs_ps4_make_video_request_at_bitrate(params, 720, 0, 0, 15000) == 0);
  assert(params[0x14 / 4] == 15000000 && params[0x10 / 4] == 13636363);
  assert(params[0x34 / 4] == 31250 && params[0x48 / 4] == 31250);
  assert(params[0x50 / 4] == 250000);
  assert(params[0x40 / 4] == 5059 && params[0x44 / 4] == 3709);
  assert(pbs_ps4_make_video_request_at_bitrate(params, 720, 0, 0, 999) == -1);
  assert(pbs_ps4_make_video_request_at_bitrate(params, 720, 0, 0, 15001) == -1);
  /* The 90kHz RTP/MSE clock must not jump when a local capture session
   * reopens: it advances one native 60fps sample for each published AU. */
  assert(pbs_ps4_pts90k(0) == 0);
  assert(pbs_ps4_pts90k(1) == 1500);
  assert(pbs_ps4_pts90k(599) == 898500);
  assert(pbs_ps4_pts90k(600) == 900000);
  assert(pbs_ps4_pts90k(601) - pbs_ps4_pts90k(600) == 1500);
  assert(pbs_ps4_make_video_request_at_rate(params, 1080, 0, 0, 30) == 0);
  assert(params[2] == 2 && params[3] == 2 && params[0x1c/4] == 30);
  assert(params[0x54/4] == 30 && params[0x58/4] == 0xff);
  assert(params[0x34/4] == 41666 && params[0x48/4] == 41666);
  assert(params[0x50/4] == 333333 && params[0x4c/4] == 20);
  assert(pbs_ps4_pts90k_at_rate(1,30) == 3000);
  assert(pbs_ps4_pts90k_at_rate(60,30) == 180000);
  assert(pbs_ps4_pts90k_at_rate(60,60) == 90000);
  assert(pbs_ps4_make_video_request_at_rate(params, 720,0,0,30) < 0);
  assert(pbs_ps4_make_video_request_at_rate(params, 1080,1,0,30) < 0);
  assert(pbs_ps4_make_video_request_at_rate(params, 1080,0,0,45) < 0);
  uint32_t expected[PBS_PS4_VIDEO_REQUEST_WORDS];
  assert(!pbs_ps4_make_video_request_at_rate(expected,1080,0,0,30));
  expected[0x30 / 4] = PBS_PS4_MEMORY_COUNT;
  assert(!pbs_ps4_make_video_request_low_memory(params,1080,0,0,30));
  assert(!memcmp(params,expected,sizeof expected));
  assert(pbs_ps4_make_video_request_low_memory(params,720,0,0,30)<0);
  assert(pbs_ps4_make_video_request_low_memory(params,1080,0,0,60)<0);
  assert(pbs_ps4_make_video_request_low_memory(params,1080,1,0,30)<0);
  assert(pbs_ps4_make_video_request_low_memory(params,1080,0,1,30)<0);
  assert(pbs_ps4_make_video_request_low_memory(NULL,1080,0,0,30)<0);
  expected[0x08 / 4] = 24;
#if PBS_PS4_NATIVE_SOURCE
  expected[0x0c / 4] = 24;
#endif
  assert(!pbs_ps4_make_video_request_remote_1080(params,1080,0,0,30));
  assert(!memcmp(params,expected,sizeof expected));
  assert(pbs_ps4_make_video_request_remote_1080(params,720,0,0,30)<0);
  assert(pbs_ps4_make_video_request_remote_1080(params,1080,0,0,60)<0);
  assert(pbs_ps4_make_video_request_remote_1080(params,1080,1,0,30)<0);
  assert(pbs_ps4_make_video_request_remote_1080(params,1080,0,1,30)<0);
  assert(pbs_ps4_make_video_request_remote_1080(NULL,1080,0,0,30)<0);
  puts("PS4 FW11 native Avcap request layouts (720p/1080p, usage0/1) verified");
  return 0;
}
