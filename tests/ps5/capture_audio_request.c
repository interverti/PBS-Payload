// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/capture_audio_request.h"
#include "common/media/profiles.h"
#include <assert.h>
#include <stdio.h>

static uint64_t read64(const unsigned char *p) { uint64_t v;memcpy(&v,p,8);return v; }
static uint32_t read32(const unsigned char *p) { uint32_t v;memcpy(&v,p,4);return v; }
int main(void) {
  const int heights[]={720,1080,1440,2160};
  for(unsigned i=0;i<4;i++) {
    pbs_capture_profile profile;
    assert(!pbs_capture_profile_for_height(heights[i],&profile));
    unsigned char guarded[0x3a];memset(guarded,0xa5,sizeof guarded);
    pbs_ps5_audio_request(guarded+1,profile.feature,0);
    const unsigned char *p=guarded+1;
    assert(guarded[0]==0xa5 && guarded[0x39]==0xa5);
    assert(read64(p)==0x38 && read64(p+8)==0x18);
    assert(read32(p+0x14)==2 && read32(p+0x18)==0);
    assert(read32(p+0x1c)==profile.feature);
    if(heights[i]==2160)assert(read32(p+0x1c)==10);
    assert(read64(p+0x20)==0x10 && read32(p+0x28)==128000);
    assert(read64(p+0x30)==0);
  }
  puts("Audio request shares granted video resource feature at every resolution, including 4K");
}
