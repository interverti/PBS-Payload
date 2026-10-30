// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/capture_resolution.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  const uint32_t legacy[]={0x03200004u,0x04000042u,0x04510001u,0x05100023u};
  const uint32_t modern[]={0x06020004u,0x09400008u,0x12400009u,0x13600007u};
  uint32_t mode;
  for (unsigned i=0;i<sizeof legacy/sizeof *legacy;i++) {
    assert(!pbs_ps5_resolution_mode(legacy[i],1080,&mode)&&mode==4);
    mode=99;
    assert(pbs_ps5_resolution_mode(legacy[i],1440,&mode)&&mode==99);
    assert(!pbs_ps5_resolution_mode(legacy[i],2160,&mode)&&mode==5);
  }
  for (unsigned i=0;i<sizeof modern/sizeof *modern;i++) {
    assert(!pbs_ps5_resolution_mode(modern[i],1440,&mode)&&mode==5);
    assert(!pbs_ps5_resolution_mode(modern[i],2160,&mode)&&mode==6);
    assert(!pbs_ps5_resolution_mode(modern[i],720,&mode)&&mode==3);
  }
  assert(pbs_ps5_resolution_mode(legacy[0],999,&mode));
  puts("Native legacy and modern resolution tables verified");
}
