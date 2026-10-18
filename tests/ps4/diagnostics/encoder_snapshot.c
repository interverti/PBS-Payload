// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/diagnostics/admission.h"
#include "platform/ps4/diagnostics/snapshot.h"
#include <ps4/kernel.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned char service[0x378], core[0xb20], backend[0x1000], driver[0x1100], codec[0x1000], sequence[0x88];
static unsigned calls;
static int fail_at, detach_failure, changed, foreign, cycle;
static const pbs_ps4_firmware *fw;
pid_t pbs_find_pid(const char *name) { assert(!strcmp(name,"SceAvCapture")); return 43; }
uint64_t pbs_process_birth_epoch(pid_t pid) { assert(pid==43); return 41; }
int kernel_dynlib_obj(pid_t pid, uint32_t handle, dynlib_obj_t *obj) {
  assert(pid==43 && !handle); obj->mapbase=0x400000;obj->mapsize=0x30000;return 0;
}
static void word(void *p, size_t off, uint64_t value) { memcpy((char *)p+off,&value,8); }
int pbs_ps4_read_snapshot(pid_t pid, uint64_t epoch,
                          const pbs_ps4_snapshot_region *r, size_t count) {
  assert(pid==43 && epoch==41 && count==1);++calls;
  if ((int)calls==fail_at) return detach_failure ? -2 : -1;
  if ((uint64_t)r->address==0x400000+fw->capture_object_head) {
    assert(r->size==8);word(r->destination,0,0x600000);
  } else if(r->address==0x600000) {
    assert(r->size==sizeof service || r->size==0x190);
    memcpy(r->destination,service,r->size);
    if(foreign) word(r->destination,0,0x400000);
    if(cycle) word(r->destination,8,0x600000);
    if(changed && calls==9) word(r->destination,0x188,0x700008);
  } else if(r->address==0x700000) {
    assert(r->size==sizeof core);memcpy(r->destination,core,r->size);
  } else if(r->address==0x800000) {
    assert(r->size==sizeof backend);memcpy(r->destination,backend,r->size);
  } else if(r->address==0x900000) {
    assert(r->size==0x1000);memcpy(r->destination,driver,r->size);
  } else if(r->address==0x901000) {
    assert(r->size==0x100);memcpy(r->destination,driver+0x1000,r->size);
  } else if(r->address==0xa00000) {
    assert(r->size==sizeof codec);memcpy(r->destination,codec,r->size);
  } else if(r->address==0xb00000) {
    assert(r->size==sizeof sequence);memcpy(r->destination,sequence,r->size);
  } else assert(0);
  return 0;
}
int main(void) {
  fw=pbs_ps4_firmware_find(0x11008001);assert(fw);
  word(service,0,0x400000+fw->capture_profile2_vtable);
  word(service,0x80,UINT64_C(0x0000000200000002));
  word(service,0x188,0x700000);word(core,0x1e8,0x800000);
  word(backend,0x4b8,0x900000);word(driver,0xd50,0xa00000);word(codec,0xa70,0xb00000);
  char path[]="/tmp/pbs-encoder-state-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);unlink(path);
  FILE *log=tmpfile();assert(log);
  assert(!pbs_ps4_encoder_snapshot(log,fw,path) && calls==9);
  FILE *out=fopen(path,"rb");assert(out);fseek(out,0,SEEK_END);
  assert(ftell(out)==(long)(8+6*16+sizeof service+sizeof core+sizeof backend+sizeof driver+sizeof codec+sizeof sequence));
  rewind(out);char magic[8];assert(fread(magic,1,8,out)==8 && !memcmp(magic,"PBSEST02",8));fclose(out);unlink(path);
  word(service,0x80,UINT64_C(0x0000000200000018));calls=0;
  assert(!pbs_ps4_encoder_snapshot(log,fw,path) && calls==9);unlink(path);
  word(service,0x80,UINT64_C(0x0000001800000018));calls=0;
#if PBS_PS4_NATIVE_SOURCE
  assert(!pbs_ps4_encoder_snapshot(log,fw,path) && calls==9);unlink(path);
#else
  assert(pbs_ps4_encoder_snapshot(log,fw,path)<0 && calls==2);
#endif
  word(service,0x80,UINT64_C(0x0000000200000002));
  calls=0;word(service,0x80,UINT64_C(0x0000001300000013));
  assert(!pbs_ps4_encoder_snapshot(log,fw,path) && calls==9);unlink(path);
  calls=0;word(service,0x80,UINT64_C(0x0000001b00000013));
  assert(pbs_ps4_encoder_snapshot(log,fw,path)<0 && calls==2);unlink(path);
  word(service,0x80,UINT64_C(0x0000000200000002));
  for(fail_at=1;fail_at<=9;++fail_at) {
    calls=0;assert(pbs_ps4_encoder_snapshot(log,fw,path)==-1 && calls==(unsigned)fail_at);
    detach_failure=1;calls=0;assert(pbs_ps4_encoder_snapshot(log,fw,path)==-2);
    detach_failure=0;
  }
  fail_at=0;calls=0;changed=1;assert(pbs_ps4_encoder_snapshot(log,fw,path)<0);changed=0;
  calls=0;foreign=1;assert(pbs_ps4_encoder_snapshot(log,fw,path)<0 && calls==2);
  calls=0;cycle=1;assert(pbs_ps4_encoder_snapshot(log,fw,path)<0 && calls==2);
  foreign=cycle=0;calls=0;word(core,0x1e8,1);assert(pbs_ps4_encoder_snapshot(log,fw,path)<0 && calls==3);
  assert(access(path,F_OK)<0);fclose(log);
  puts("Encoder failure snapshot: bounded pointers, profile ownership, exact sizes, read/detach failures and changed handle refused");
}
