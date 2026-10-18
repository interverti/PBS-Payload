// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/diagnostics/venc_journal.h"
#include "common/process/inject.h"
#include <ps4/kernel.h>
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned char target[0x20000];
static int attached,writes,fail_write,resolve_fail,detach_fail;
static const uint64_t original=UINT64_C(0x800900000),base=0x400000;
pid_t pbs_find_pid(const char *name) { assert(!strcmp(name,"SceAvCapture"));return 43; }
uint64_t pbs_process_birth_epoch(pid_t pid) { assert(pid==43);return 41; }
int kernel_dynlib_handle(pid_t pid,const char *name,uint32_t *handle) {
  assert(pid==43 && !strcmp(name,"libSceVencCore.sprx"));*handle=8;return 0;
}
int kernel_dynlib_obj(pid_t pid,uint32_t handle,dynlib_obj_t *obj) {
  assert(pid==43 && handle==8);obj->mapbase=base;obj->mapsize=sizeof target;return 0;
}
intptr_t pbs_resolve_in(pid_t pid,const char *name,const char *symbol) {
  assert(pid==43 && !strcmp(name,"libSceLibcInternal.sprx") && !strcmp(symbol,"printf"));
  return resolve_fail?0:(intptr_t)original;
}
int ptrace(int request,pid_t pid,caddr_t address,int data) {
  assert(pid==43 && !data);
  if(request==PT_ATTACH) {assert(!attached);attached=1;return 0;}
  assert(attached);
  if(request==PT_DETACH) {if(detach_fail)return -1;attached=0;return 0;}
  assert(request==PT_IO);struct ptrace_io_desc *io=(void *)address;
  size_t off=(uintptr_t)io->piod_offs-base;
  assert(off<sizeof target && io->piod_len<=4096 && off+io->piod_len<=sizeof target);
  if(io->piod_op==PIOD_READ_D) memcpy(io->piod_addr,target+off,io->piod_len);
  else {
    assert(io->piod_op==PIOD_WRITE_D);++writes;
    assert(off==0x17400 || off==0x184e8); /* No encoder/flags/data writes. */
    if(writes==fail_write) {
      memcpy(target+off,io->piod_addr,io->piod_len/2);return -1;
    }
    memcpy(target+off,io->piod_addr,io->piod_len);
  }
  return 0;
}
pid_t waitpid(pid_t pid,int *status,int options) {assert(pid==43 && attached && options==(WNOHANG|WUNTRACED));*status=(SIGSTOP<<8)|0x7f;return pid;}
int usleep(useconds_t delay) {assert(delay==5000);return 0;}
int kill(pid_t pid,int signal) {assert(pid==43 && signal==SIGCONT);return 0;}
static uint64_t pointer(void) {uint64_t n;memcpy(&n,target+0x184e8,8);return n;}
static void fresh(void) {
  assert(!attached && !pbs_ps4_venc_journal_pending());memset(target,0,sizeof target);
  writes=fail_write=resolve_fail=0;
  const unsigned char plt[]={0xff,0x25,0x0a,0x84,0x01,0x00,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc};
  const unsigned char start[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x85,0xff,0x74,0x75};
  const unsigned char seq[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec};
  const unsigned char pic[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec};
  memcpy(target+0xd8,plt,16);memcpy(target+0x20b0,start,16);memcpy(target+0x75e0,seq,16);memcpy(target+0xc150,pic,16);
  memcpy(target+0x184e8,&original,8);
}
static void exercise_stub(void) {
  unsigned char *mem=mmap(NULL,12288,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  assert(mem!=MAP_FAILED);memset(mem+4096+PBS_VENC_JOURNAL_DATA_SIZE,0x5a,32);
  size_t bytes=pbs_ps4_venc_journal_stub(mem,(uintptr_t)(mem+4096));assert(bytes<96);
  int (*fn)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)=(void *)mem;
  for(unsigned i=0;i<70;++i) assert(fn(1000+i,2000+i,3000+i,4000+i,5000+i,6000+i)==0);
  uint32_t count;memcpy(&count,mem+4096,4);assert(count==70);
  for(unsigned i=6;i<70;++i) {
    uint64_t *row=(void *)(mem+4096+8+(i&63)*64);
    assert(row[0]==i+1);
    for(unsigned k=1;k<=6;++k) assert(row[k]==k*1000+i);
  }
  for(unsigned i=0;i<32;++i) assert(mem[4096+PBS_VENC_JOURNAL_DATA_SIZE+i]==0x5a);
  munmap(mem,12288);
}
int main(void) {
  exercise_stub();FILE *log=tmpfile();assert(log);
  const pbs_ps4_firmware *fw=pbs_ps4_firmware_find(0x11008001);
  char path[]="/tmp/pbs-venc-journal-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);unlink(path);
  fresh();target[0xd8]^=1;assert(pbs_ps4_venc_journal_apply(log,fw)<0 && !writes && !attached);
  fresh();target[0x1c200]=1;assert(pbs_ps4_venc_journal_apply(log,fw)<0 && !writes);
  fresh();resolve_fail=1;assert(pbs_ps4_venc_journal_apply(log,fw)<0 && !writes);
  fresh();fail_write=1;assert(pbs_ps4_venc_journal_apply(log,fw)<0 && !pbs_ps4_venc_journal_pending() && pointer()==original);
  fresh();fail_write=2;assert(pbs_ps4_venc_journal_apply(log,fw)<0 && !pbs_ps4_venc_journal_pending() && pointer()==original && !attached);
  fresh();assert(!pbs_ps4_venc_journal_apply(log,fw));assert(pointer()==base+0x17400 && pbs_ps4_venc_journal_pending());
  uint32_t count=2;memcpy(target+0x1c200,&count,4);
  assert(!pbs_ps4_venc_journal_finish(log,path));assert(pointer()==original && !pbs_ps4_venc_journal_pending() && !attached);
  assert(target[0x17400]==0x49); /* Inert stub retained for in-flight return. */
  FILE *out=fopen(path,"rb");assert(out);fseek(out,0,SEEK_END);assert(ftell(out)==(long)(48+PBS_VENC_JOURNAL_DATA_SIZE));fclose(out);unlink(path);
  /* A second install without a reboot refuses the nonzero padding. */
  writes=0;assert(pbs_ps4_venc_journal_apply(log,fw)<0 && !writes);
  fresh();assert(!pbs_ps4_venc_journal_apply(log,fw));
  fail_write=writes+1; /* Torn restore cannot be resumed as an invalid pointer. */
  assert(pbs_ps4_venc_journal_finish(log,path)<0 && attached && pbs_ps4_venc_journal_pending());
  fclose(log);puts("Venc printf journal: real AMD64 ring/arguments/wrap, signatures, padding refusal, import rollback, restore and torn-pointer quarantine verified");
}
