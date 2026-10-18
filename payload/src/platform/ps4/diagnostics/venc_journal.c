// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/diagnostics/venc_journal.h"
#include "common/process/inject.h"
#include <ps4/kernel.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
/* These padding ranges are beyond this exact ELF's RX filesz=0x173d0 and
 * RW memsz=0x1c1a8, but within their mapped 16KiB pages. No live object owns
 * them. Refuse any other image signature, nonzero padding or smaller map. */
#define CODE_RVA 0x17400u
#define DATA_RVA 0x1c200u
#define PRINTF_GOT_RVA 0x184e8u
static struct {
  pid_t pid;
  uint64_t epoch, base, original_printf;
  unsigned pending, detach_uncertain;
} owner;
int pbs_ps4_venc_journal_pending(void) { return owner.pending != 0 || owner.detach_uncertain != 0; }
size_t pbs_ps4_venc_journal_stub(unsigned char *out, uint64_t address) {
  /* SysV AMD64 leaf: only caller-saved GPRs/flags change; no stack, SIMD,
   * syscall or native function is borrowed. Publish ticket after all args.
   * Suppresses printf output, returns zero; debug printf results are ignored. */
  static const unsigned char code[] = {
    0x49,0xbb,0,0,0,0,0,0,0,0, /* movabs r11,data */
    0xb8,1,0,0,0,              /* mov eax,1 */
    0xf0,0x41,0x0f,0xc1,0x03, /* lock xadd [r11],eax */
    0x41,0x89,0xc2,           /* mov r10d,eax */
    0x83,0xe0,0x3f,           /* and eax,63 */
    0x48,0xc1,0xe0,6,         /* shl rax,6 */
    0x4d,0x8d,0x5c,0x03,8,    /* lea r11,[r11+rax+8] */
    0x49,0xc7,0x03,0,0,0,0,   /* invalidate wrapped record before args */
    0x49,0x89,0x7b,8,         /* fmt */
    0x49,0x89,0x73,16,        /* rsi */
    0x49,0x89,0x53,24,        /* rdx */
    0x49,0x89,0x4b,32,        /* rcx */
    0x4d,0x89,0x43,40,        /* r8 */
    0x4d,0x89,0x4b,48,        /* r9 */
    0x41,0x83,0xc2,1,         /* add r10d,1 */
    0x4d,0x89,0x13,           /* commit ticket */
    0x31,0xc0,0xc3
  };
  memcpy(out, code, sizeof code);
  memcpy(out + 2, &address, sizeof address);
  return sizeof code;
}
static int transfer(uint64_t address, void *bytes, size_t size, int write) {
  if (!address || address > INT64_MAX - size) return -1;
  for (size_t offset=0; offset<size;) {
    size_t length=size-offset; if(length>4096) length=4096;
    struct ptrace_io_desc io={.piod_op=write?PIOD_WRITE_D:PIOD_READ_D,
      .piod_offs=(void *)(uintptr_t)(address+offset),
      .piod_addr=(unsigned char *)bytes+offset,.piod_len=length};
    if(ptrace(PT_IO,owner.pid,(caddr_t)&io,0) || io.piod_len!=length) return -1;
    offset+=length;
  }
  return 0;
}
static int stop_daemon(void) {
  if(pbs_process_birth_epoch(owner.pid)!=owner.epoch ||
      ptrace(PT_ATTACH,owner.pid,NULL,0)) return -1;
  for(unsigned ms=0;ms<250;ms+=5) {
    int status=0;pid_t got=waitpid(owner.pid,&status,WNOHANG|WUNTRACED);
    if(got==owner.pid) return WIFSTOPPED(status) &&
        pbs_process_birth_epoch(owner.pid)==owner.epoch ? 0 : -2;
    if(got<0 && errno!=EINTR) return -2;
    usleep(5000);
  }
  return -2; /* Caller must detach after an attached-but-uncertain stop. */
}
static int resume_daemon(void) {
  for(unsigned retry=0;retry<3;++retry) {
    if(!ptrace(PT_DETACH,owner.pid,NULL,0)) return 0;
    usleep(5000);
  }
  owner.detach_uncertain=1;kill(owner.pid,SIGCONT);return -1;
}
static int module_unchanged(void) {
  uint32_t handle=0;dynlib_obj_t obj={0};
  return pbs_process_birth_epoch(owner.pid)!=owner.epoch ||
      kernel_dynlib_handle(owner.pid,"libSceVencCore.sprx",&handle) ||
      kernel_dynlib_obj(owner.pid,handle,&obj) || obj.mapbase!=owner.base ||
      obj.mapsize<0x20000 ? -1 : 0;
}
static int restore_import_paused(void) {
  uint64_t verify=0;
  /* Never short-circuit the verification on a reported write error: PT_IO
   * can complete or partially store before reporting failure. */
  transfer(owner.base+PRINTF_GOT_RVA,&owner.original_printf,8,1);
  if(!transfer(owner.base+PRINTF_GOT_RVA,&verify,8,0)) {
    if(verify==owner.original_printf) { owner.pending=0;return 0; }
    if(verify==owner.base+CODE_RVA) return -1; /* Intact self-contained stub. */
  }
  /* An unverifiable/torn import must never be executed. Keep the daemon
   * stopped; the payload reports quarantine and requests a complete reboot. */
  owner.detach_uncertain=1;return -1;
}
int pbs_ps4_venc_journal_apply(FILE *log, const pbs_ps4_firmware *fw) {
  const pbs_ps4_firmware *known=pbs_ps4_firmware_find(kernel_get_fw_version());
  if(!log || !fw || !known || fw->version!=known->version || pbs_ps4_venc_journal_pending()) return -1;
  pid_t pid=pbs_find_pid("SceAvCapture");
  uint64_t epoch=pid>0?pbs_process_birth_epoch(pid):0;
  uint32_t handle=0;dynlib_obj_t obj={0};
  if(!epoch || kernel_dynlib_handle(pid,"libSceVencCore.sprx",&handle) ||
      kernel_dynlib_obj(pid,handle,&obj) || !obj.mapbase || obj.mapsize<0x20000 ||
      obj.mapbase>INT64_MAX-obj.mapsize) return -1;
  owner.pid=pid;owner.epoch=epoch;owner.base=obj.mapbase;
  int stopped=stop_daemon();
  if(stopped) { if(stopped==-2) resume_daemon();return -1; }
  const char *stage="module";int result=-1;
  unsigned char data[PBS_VENC_JOURNAL_DATA_SIZE],stub[96],check[96];
  size_t length=pbs_ps4_venc_journal_stub(stub,owner.base+DATA_RVA);
  static const struct { unsigned rva; unsigned char bytes[16]; } signatures[]={
    {0xd8,{0xff,0x25,0x0a,0x84,0x01,0x00,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}},
    {0x20b0,{0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x85,0xff,0x74,0x75}},
    {0x75e0,{0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec}},
    {0xc150,{0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec}}
  };
  if(module_unchanged()) goto done;
  stage="signatures";
  for(unsigned i=0;i<sizeof signatures/sizeof signatures[0];++i)
    if(transfer(owner.base+signatures[i].rva,check,16,0) ||
        memcmp(check,signatures[i].bytes,16)) goto done;
  stage="unused-padding";
  if(transfer(owner.base+CODE_RVA,check,length,0) ||
      transfer(owner.base+DATA_RVA,data,sizeof data,0)) goto done;
  for(size_t i=0;i<length;++i) if(check[i]) {
    fprintf(log,"Venc journal padding occupied: rva=%llx byte=%02x\n",
        (unsigned long long)(CODE_RVA+i),check[i]);goto done;
  }
  for(size_t i=0;i<sizeof data;++i) if(data[i]) {
    fprintf(log,"Venc journal padding occupied: rva=%llx byte=%02x\n",
        (unsigned long long)(DATA_RVA+i),data[i]);goto done;
  }
  stage="printf-target";
  if(transfer(owner.base+PRINTF_GOT_RVA,&owner.original_printf,8,0) ||
      owner.original_printf<0x10000 || owner.original_printf>=INT64_MAX) goto done;
  intptr_t expected=pbs_resolve_in(pid,"libSceLibcInternal.sprx","printf");
  if(!expected || (uint64_t)expected!=owner.original_printf) {
    fprintf(log,"Venc journal printf mismatch: got=%llx expected=%llx mapsize=%llx\n",
        (unsigned long long)owner.original_printf,(unsigned long long)expected,
        (unsigned long long)obj.mapsize);goto done;
  }
  stage="install-stub";
  if(transfer(owner.base+CODE_RVA,stub,length,1) ||
      transfer(owner.base+CODE_RVA,check,length,0) || memcmp(stub,check,length)) goto done;
  stage="install-import";
  uint64_t entry=owner.base+CODE_RVA,verify=0;
  owner.pending=1; /* A failed PT_IO may have partially stored the pointer. */
  if(transfer(owner.base+PRINTF_GOT_RVA,&entry,8,1) ||
      transfer(owner.base+PRINTF_GOT_RVA,&verify,8,0) || verify!=entry) goto done;
  result=0;
done:
  if(result && owner.pending) restore_import_paused();
  if(!owner.detach_uncertain && resume_daemon()) result=-1;
  fprintf(log,"Venc journal apply: rc=%d pending=%u uncertain=%u stage=%s base=%llx; printf only, encoder checks unchanged\n",
      result,owner.pending,owner.detach_uncertain,stage,(unsigned long long)owner.base);
  return result;
}
int pbs_ps4_venc_journal_finish(FILE *log, const char *path) {
  if(owner.detach_uncertain) return -1;
  if(!owner.pending) return 0;
  if(!log || !path) return -1;
  int stopped=stop_daemon();
  if(stopped) { if(stopped==-2) resume_daemon();return -1; }
  unsigned char data[PBS_VENC_JOURNAL_DATA_SIZE];
  uint64_t target=0;int result=-1,read_ok=0;
  if(module_unchanged()) goto done;
  /* Restore the exact original import, even after a partial install write.
   * The code stub and its data stay mapped/inert: never erase instructions
   * that a paused thread might still be executing (thread enumeration absent). */
  if(transfer(owner.base+PRINTF_GOT_RVA,&target,8,0)) goto done;
  if(target!=owner.original_printf && target!=owner.base+CODE_RVA) {
    unsigned char got[8],before[8],after[8];
    uint64_t entry=owner.base+CODE_RVA;
    memcpy(got,&target,8);memcpy(before,&owner.original_printf,8);memcpy(after,&entry,8);
    for(unsigned i=0;i<8;++i) if(got[i]!=before[i] && got[i]!=after[i]) goto done;
  }
  if(restore_import_paused()) goto done;
  read_ok=!transfer(owner.base+DATA_RVA,data,sizeof data,0);
  result=0;
done:
  if(!owner.detach_uncertain && resume_daemon()) result=-1;
  fprintf(log,"Venc journal restore: rc=%d pending=%u uncertain=%u original_printf=%llx; inert padding retained until reboot\n",
      result,owner.pending,owner.detach_uncertain,(unsigned long long)owner.original_printf);
  if(!result && !read_ok) {
    fprintf(log,"Venc journal read failed after verified import restoration\n");
    return -1;
  }
  if(!result && read_ok) {
    FILE *out=fopen(path,"wb");
    if(!out) return -1;
    uint64_t header[5]={owner.base,(uint64_t)owner.pid,owner.epoch,owner.original_printf,sizeof data};
    int failed=fwrite("PBSVJNL1",1,8,out)!=8 || fwrite(header,1,sizeof header,out)!=sizeof header ||
        fwrite(data,1,sizeof data,out)!=sizeof data;
    if(fclose(out)) failed=1;
    uint32_t count=0;memcpy(&count,data,4);
    fprintf(log,"Venc journal file: rc=%d path=%s total=%u\n",failed?-1:0,path,count);
    if(failed) return -1;
  }
  return result;
}
