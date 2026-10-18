// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/diagnostics/latch_patch.h"
#include <ps4/kernel.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef PBS_PS4_BUDGET_UNLOCK
#define PBS_PS4_BUDGET_UNLOCK 0
#endif
#if PBS_PS4_NATIVE_SOURCE
#define TEST_SITE 0x9d7du
#define TEST_BYTE 0x9d7fu
#define TEST_FLAGS 0x300004u
#define TEST_OPCODE 0x35
#else
#define TEST_SITE 0x9d9cu
#define TEST_BYTE 0x9d9du
#define TEST_FLAGS 0x100004u
#define TEST_OPCODE 0x25
#endif
static unsigned char memory[0x30000];
static int attached, writes, fail_write_at, detach_fail, identity_fail;
static int fail_budget_writes;
static unsigned write_offsets[64];
static const pbs_ps4_firmware *fw;
pid_t pbs_find_pid(const char *name) { assert(!strcmp(name,"SceAvCapture")); return 43; }
uint64_t pbs_process_birth_epoch(pid_t pid) { assert(pid==43); return identity_fail ? 42 : 41; }
int kernel_dynlib_obj(pid_t pid, uint32_t handle, dynlib_obj_t *out) {
  assert(pid==43 && !handle); out->mapbase=0x400000;out->mapsize=sizeof memory;return 0;
}
int ptrace(int request, pid_t pid, caddr_t address, int data) {
  assert(pid==43);
  if(request==PT_ATTACH){assert(!attached && !data);attached=1;return 0;}
  assert(attached);
  if(request==PT_DETACH){if(detach_fail){--detach_fail;return -1;}attached=0;return 0;}
  assert(request==PT_IO && !data);
  struct ptrace_io_desc *io=(void *)address;
  uintptr_t offset=(uintptr_t)io->piod_offs-0x400000;
  assert(offset<sizeof memory && io->piod_len<=16);
  if(io->piod_op==PIOD_READ_D){memcpy(io->piod_addr,memory+offset,io->piod_len);return 0;}
  assert(io->piod_op==PIOD_WRITE_D);
  assert((offset==TEST_BYTE && io->piod_len==1) ||
         (offset==fw->capture_flags && io->piod_len==4));
  assert(writes<64);write_offsets[writes++]=(unsigned)offset;
  int failing=(fail_budget_writes && offset==fw->capture_flags && io->piod_len==4) ||
              (fail_write_at && writes==fail_write_at);
  memcpy(memory+offset,io->piod_addr,io->piod_len);
  return failing ? -1 : 0;
}
pid_t waitpid(pid_t pid,int *status,int options){assert(pid==43 && attached && options==(WNOHANG|WUNTRACED));*status=(SIGSTOP<<8)|0x7f;return pid;}
int usleep(useconds_t delay){assert(delay==5000);return 0;}
int kill(pid_t pid,int signal){assert(pid==43 && signal==SIGCONT);return 0;}
static void original(void){
  assert(!attached && !pbs_ps4_latch_patch_pending());
  writes=fail_write_at=identity_fail=0;
  memset(memory,0,sizeof memory);
  memcpy(memory+fw->capture_caps,fw->capture_caps_signature,16);
  for(unsigned i=0;i<PBS_PS4_LATCH_SITE_COUNT;++i)memcpy(memory+fw->capture_latch_sites[i].offset,fw->capture_latch_sites[i].before,fw->capture_latch_sites[i].size);
  *(uint32_t *)(memory+fw->capture_flags)=TEST_FLAGS;
}
static void check_original(void){
  assert(!attached && !pbs_ps4_latch_patch_pending());
  for(unsigned i=0;i<PBS_PS4_LATCH_SITE_COUNT;++i)assert(!memcmp(memory+fw->capture_latch_sites[i].offset,fw->capture_latch_sites[i].before,fw->capture_latch_sites[i].size));
  assert(*(uint32_t *)(memory+fw->capture_flags)==TEST_FLAGS);
}
int main(void){
  fw=pbs_ps4_firmware_find(0x11008001);
#if PBS_PS4_NATIVE_SOURCE
  static pbs_ps4_firmware source_fw;
  source_fw=*fw;
  source_fw.capture_latch_sites[0]=(pbs_ps4_text_site){0x9d7d,5,{0x35,0,2,0,0},{0x35,0,0,0,0}};
  fw=&source_fw;
#endif
  FILE *log=tmpfile();assert(log);
  original();
#if PBS_PS4_BUDGET_UNLOCK
  /* Budget self-provisions the process state without a prior latch apply. */
  assert(!pbs_ps4_latch_budget_set(log,1) && pbs_ps4_latch_budget_pending());
  assert(*(uint32_t *)(memory+fw->capture_flags)==0x100204);
  assert(!pbs_ps4_latch_budget_set(log,0) && !pbs_ps4_latch_budget_pending());
  check_original();
#endif
  original();assert(!pbs_ps4_latch_patch_apply(log,fw));assert(pbs_ps4_latch_patch_pending());
  assert(writes==1 && write_offsets[0]==TEST_BYTE);
  assert(pbs_ps4_latch_patch_apply(log,fw)<0); /* No adoption/reapply. */
  assert(!pbs_ps4_latch_patch_restore(log));check_original();
  assert(writes==2 && write_offsets[1]==TEST_BYTE);
  original();fail_write_at=1;assert(pbs_ps4_latch_patch_apply(log,fw)<0);check_original();
  original();memory[fw->capture_caps]^=1;assert(pbs_ps4_latch_patch_apply(log,fw)<0 && !writes);
  original();memory[TEST_SITE]=0xcc;assert(pbs_ps4_latch_patch_apply(log,fw)<0 && !writes);
  original();*(uint32_t *)(memory+fw->capture_flags)=0x140004;assert(pbs_ps4_latch_patch_apply(log,fw)<0 && !writes);
  original();*(uint32_t *)(memory+fw->capture_slots)=1;assert(pbs_ps4_latch_patch_apply(log,fw)<0 && !writes);
  original();assert(!pbs_ps4_latch_patch_apply(log,fw));
  memory[TEST_SITE]=0xcc;int before=writes;
  assert(pbs_ps4_latch_patch_restore(log)<0 && writes==before && pbs_ps4_latch_patch_pending());
  memory[TEST_SITE]=TEST_OPCODE;assert(!pbs_ps4_latch_patch_restore(log));check_original();
  original();assert(!pbs_ps4_latch_patch_apply(log,fw));
  fail_write_at=writes+1;assert(!pbs_ps4_latch_patch_restore(log));check_original(); /* Partial restore retried. */
  original();detach_fail=1;assert(!pbs_ps4_latch_patch_apply(log,fw) && pbs_ps4_latch_patch_pending());
  assert(!pbs_ps4_latch_patch_restore(log));check_original();
#if PBS_PS4_BUDGET_UNLOCK
  /* Budget arm works from stale-but-valid process state with a fresh idle. */
  original();assert(!pbs_ps4_latch_budget_set(log,1));
  assert(writes==1 && write_offsets[0]==(unsigned)fw->capture_flags);
  assert(*(uint32_t *)(memory+fw->capture_flags)==0x100204);
  assert(pbs_ps4_latch_budget_pending() && !pbs_ps4_latch_patch_pending());
  assert(!pbs_ps4_latch_budget_set(log,1) && writes==1); /* idempotent arm */
  assert(!pbs_ps4_latch_budget_set(log,0));
  assert(writes==2 && write_offsets[1]==(unsigned)fw->capture_flags);
  assert(!pbs_ps4_latch_budget_pending());
  /* Full cycle: arm after latch apply; capture_end clears budget, Open's
   * window ends by the lone latch restored. */
  original();assert(!pbs_ps4_latch_patch_apply(log,fw) && pbs_ps4_latch_patch_pending());
  assert(!pbs_ps4_latch_budget_set(log,1));
  assert(writes==2 && write_offsets[1]==(unsigned)fw->capture_flags);
  assert(*(uint32_t *)(memory+fw->capture_flags)==0x100204);
  assert(!pbs_ps4_latch_budget_set(log,1) && writes==2);
  assert(!pbs_ps4_latch_budget_set(log,0));
  assert(writes==3 && write_offsets[2]==(unsigned)fw->capture_flags);
  assert(*(uint32_t *)(memory+fw->capture_flags)==TEST_FLAGS);
  assert(!pbs_ps4_latch_patch_restore(log));
  assert(writes==4 && write_offsets[3]==0x9d9d);
  check_original();
  /* Foreign owner state: arm refused without any write. */
  original();assert(!pbs_ps4_latch_patch_apply(log,fw));
  *(uint32_t *)(memory+fw->capture_flags)=0x140004;
  int refused=writes;assert(pbs_ps4_latch_budget_set(log,1)<0 && writes==refused);
  *(uint32_t *)(memory+fw->capture_flags)=TEST_FLAGS;
  assert(!pbs_ps4_latch_patch_restore(log));check_original();
  /* Stale armed leftover from a lost run: adopted, then cleared on restore. */
  original();assert(!pbs_ps4_latch_patch_apply(log,fw));
  *(uint32_t *)(memory+fw->capture_flags)=0x100204;
  assert(!pbs_ps4_latch_budget_set(log,1) && writes==2 &&
         write_offsets[1]==(unsigned)fw->capture_flags);
  assert(*(uint32_t *)(memory+fw->capture_flags)==0x100204);
  assert(pbs_ps4_latch_budget_pending());
  assert(!pbs_ps4_latch_budget_set(log,0));
  assert(!pbs_ps4_latch_budget_pending());
  assert(!pbs_ps4_latch_patch_restore(log));check_original();
  /* Budget clear retries survive failing flag writes: the failing io still
   * transfers the value, so the bit is clean while ownership stays armed. */
  original();assert(!pbs_ps4_latch_patch_apply(log,fw));
  assert(!pbs_ps4_latch_budget_set(log,1));
  fail_budget_writes=1;
  assert(pbs_ps4_latch_budget_set(log,0)<0);  /* every clear io fails */
  fail_budget_writes=0;
  assert(pbs_ps4_latch_budget_pending());
  assert(*(uint32_t *)(memory+fw->capture_flags)==TEST_FLAGS);
  assert(!memcmp(memory+fw->capture_latch_sites[0].offset,fw->capture_latch_sites[0].after,5));
  assert(!pbs_ps4_latch_budget_set(log,0));
  assert(!pbs_ps4_latch_budget_pending());
  assert(!pbs_ps4_latch_patch_restore(log));check_original();
  /* A live session keeps daemon-set flag bits: restore only clears 0x200. */
  original();assert(!pbs_ps4_latch_patch_apply(log,fw));
  assert(!pbs_ps4_latch_budget_set(log,1));
  *(uint32_t *)(memory+fw->capture_flags)=0x170204;
  assert(!pbs_ps4_latch_budget_set(log,0));
  assert(*(uint32_t *)(memory+fw->capture_flags)==0x170004);
  assert(!pbs_ps4_latch_budget_pending());
  assert(!pbs_ps4_latch_patch_restore(log));
  assert(*(uint32_t *)(memory+fw->capture_flags)==0x170004);
  assert(!memcmp(memory+fw->capture_latch_sites[0].offset,fw->capture_latch_sites[0].before,5));
  assert(!pbs_ps4_latch_budget_pending() && !pbs_ps4_latch_patch_pending());
  /* Persistent arm-write failure: armed leftover stays unowned on handoff. */
  original();assert(!pbs_ps4_latch_patch_apply(log,fw));
  fail_budget_writes=1;
  assert(pbs_ps4_latch_budget_set(log,1)<0);
  fail_budget_writes=0;
  assert(*(uint32_t *)(memory+fw->capture_flags)==0x100204);
  assert(pbs_ps4_latch_patch_pending()); /* latch bytes owned; budget expelled */
  *(uint32_t *)(memory+fw->capture_flags)=TEST_FLAGS; /* simulated operator reset */
  assert(!pbs_ps4_latch_patch_restore(log));check_original();
#endif
  fclose(log);puts("Latch patch: single immediate byte, instruction signatures, idle ownership, rollback, restoration, budget arm/adopt/clear verified");
}
