// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/system/layout.h"
#include "platform/ps4/system/trampoline.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *image;
static size_t image_size;
static uint64_t image_base;
static int read_image(void *opaque, uint64_t at, void *out, size_t size) {
  (void)opaque;
  if (at<image_base || size>image_size || at-image_base>image_size-size) return -1;
  memcpy(out,image+(size_t)(at-image_base),size);
  return 0;
}
static void import_run(size_t at, unsigned count) {
  for (unsigned i=0;i<count;++i) {
    size_t stub=at+i*16;
    image[stub]=0xff; image[stub+1]=0x25;
    int32_t delta=(int32_t)(0x9000-(stub+6));
    memcpy(image+stub+2,&delta,4);
    memset(image+stub+6,0xcc,10);
  }
  uint64_t fn=image_base+0x1000;
  memcpy(image+0x9000,&fn,8);
}

int main(int argc, char **argv) {
  if (argc==2 || argc==4) {
    FILE *f=fopen(argv[argc==2?1:2],"rb"); assert(f);
    assert(!fseek(f,0,SEEK_END)); long length=ftell(f); assert(length>0);
    rewind(f); image_size=(size_t)length;
    image=malloc(image_size); assert(image);
    assert(fread(image,1,image_size,f)==image_size); fclose(f);
    if (argc==4) {
      image_base=0x10000000;
      intptr_t trap=pbs_ps4_layout_trap(read_image,NULL,(intptr_t)(image_base+strtoull(argv[3],NULL,0)));
      if (trap) printf("trap=%llx\n",(unsigned long long)(trap-image_base));
      free(image); return trap?0:1;
    }
    image_base=UINT64_C(0xffffffff80000000);
    pbs_ps4_firmware layout;
    int rc=pbs_ps4_layout_kernel(read_image,NULL,image_base,image_size,&layout);
    if (!rc) printf("acmgr=%x allow=%x policy=%x jump=%02x%02x%02x%02x%02x\n",
      layout.acmgr_offset,layout.gates[0].offset,layout.gates[1].offset,
      layout.gates[1].after[0],layout.gates[1].after[1],layout.gates[1].after[2],
      layout.gates[1].after[3],layout.gates[1].after[4]);
    free(image); return rc?1:0;
  }
  image_size=0x30000; image=calloc(1,image_size); assert(image);
  image_base=0x10000000;
  pbs_ps4_firmware result;
  memset(&result,0xa5,sizeof result);
  assert(pbs_ps4_layout_kernel(read_image,NULL,image_base,image_size,&result)<0);
  assert(result.gates[0].offset==0xa5a5a5a5u); /* Never publish partial output. */
  intptr_t anchor=(intptr_t)(image_base+0x5000);
  assert(!pbs_ps4_layout_trap(read_image,NULL,anchor));
  import_run(0x4123,PBS_PS4_CALL_SLOTS+3);
  assert(pbs_ps4_layout_trap(read_image,NULL,anchor)==(intptr_t)(image_base+0x4129));
  image[0x4123+6]=0x90; /* First padding slot unavailable: next complete run. */
  assert(pbs_ps4_layout_trap(read_image,NULL,anchor)==(intptr_t)(image_base+0x4139));
  image[0x4123+6]=0xcc;
  import_run(0x7000,PBS_PS4_CALL_SLOTS);
  assert(!pbs_ps4_layout_trap(read_image,NULL,anchor)); /* Ambiguous runs. */
  memset(image+0x7000,0,PBS_PS4_CALL_SLOTS*16);
  memset(image+0x9000,0,8);
  assert(!pbs_ps4_layout_trap(read_image,NULL,anchor)); /* Invalid GOT. */
  memset(image,0,image_size);
  import_run(0x3ff3,PBS_PS4_CALL_SLOTS); /* Unaligned, across scanner blocks. */
  assert(pbs_ps4_layout_trap(read_image,NULL,anchor)==(intptr_t)(image_base+0x3ff9));
  free(image);
  puts("Runtime layout: malformed image refusal, unchanged failed output, import/GOT validation, ambiguous/modified padding and cross-page discovery verified");
  return 0;
}
