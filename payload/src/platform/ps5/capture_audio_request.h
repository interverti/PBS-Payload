// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <string.h>

/* OpenAudio must use the resource feature already granted for this video
 * session: in particular, 2160p owns feature 10, not the 1080p feature 12. */
static inline void pbs_ps5_audio_request(unsigned char params[0x38],
                                        unsigned capture_feature,uint32_t format) {
  memset(params,0,0x38);
  const uint64_t size=0x38,common=0x18,rate_size=0x10;
  const uint32_t usage=2,rate=128000,feature=capture_feature;
  memcpy(params,&size,8);
  memcpy(params+8,&common,8);
  memcpy(params+0x14,&usage,4);
  memcpy(params+0x18,&format,4);
  memcpy(params+0x1c,&feature,4);
  memcpy(params+0x20,&rate_size,8);
  memcpy(params+0x28,&rate,4);
}
