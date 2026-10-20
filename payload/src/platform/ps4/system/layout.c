// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps4/system/layout.h"
#include "platform/ps4/system/trampoline.h"
#include <elf.h>
#include <limits.h>
#include <string.h>

static int match(const unsigned char *b, const unsigned char *p, const char *mask) {
  for (size_t i=0; mask[i]; ++i) if (mask[i]=='x' && b[i]!=p[i]) return 0;
  return 1;
}
static int relative(uint64_t at, const unsigned char *p, uint64_t *target) {
  int32_t delta;
  memcpy(&delta,p,4);
  if (delta<0 && at<(uint64_t)-(int64_t)delta) return -1;
  if (delta>0 && at>UINT64_MAX-(uint32_t)delta) return -1;
  *target=delta<0 ? at-(uint64_t)-(int64_t)delta : at+(uint32_t)delta;
  return 0;
}
static int inside(uint64_t address, uint64_t start, size_t size, size_t need) {
  return address>=start && need<=size && address-start<=size-need;
}

/* Request-bit whitelist and subsequent credential check in kern_ptrace.
 * Mask only relocations, branch distances and stack frame displacements.
 * Accept original or already-armed first branch, then reconstruct its jump. */
static const unsigned char allow_pattern[] =
  "\x83\xf8\x2a\x77\x1c\x48\xb8\x36\x10\x00\x00\x7e\x02\x00\x00"
  "\x4c\x0f\xa3\xe0\x0f\x83\x02\x02\x00\x00\x85\xdb\x0f\x84\x19\x02\x00\x00"
  "\x4c\x8b\xb5\xb8\xfe\xff\xff\x49\x8b\x46\x08\x48\x8b\x78\x40\xe8\x49\xc4\x04\x00";
static const char allow_mask[] =
  "xxx??xxxxxxxxxxxxxxxx????xxxx????xxx????xxxxxxxxx????";
_Static_assert(sizeof allow_mask == sizeof allow_pattern, "allow signature mask");
static const unsigned char policy_pattern[] =
  "\x41\x83\xff\x0b\x41\xbc\x10\x00\x00\x00\x75\x0d\x49\x81\xbe\x30\x04\x00\x00\xff\xff\xff\x7f\x77\x6d"
  "\x48\x8b\x41\x08\x44\x89\xc3\x48\x8b\x78\x40\xe8\x7f\xbf\x04\x00\x41\x8b\x8e\xa8\x00\x00\x00"
  "\x41\x89\xd8\x85\xdb\x75\x5f\x41\x83\xff\x0b\x75\x59\x85\xc0\x74\x55\xf7\xc1\x00\x08\x00\x00\x75\x5d"
  "\x4c\x89\xf7\xe8\x07\x4f\xce\xff\x41\x89\xd8\x41\xbf\x07\x00\x00\x00\x45\x85\xc0\x0f\x85\x37\x02\x00\x00";

static int policy_match(const unsigned char *p) {
  for (unsigned i=0;i<sizeof policy_pattern-1;++i) {
    if ((i>=25 && i<30) || (i>=37 && i<41) || (i>=77 && i<81) || i>=95 ||
        i==11 || i==24 || i==54 || i==60 || i==64 || i==72) continue;
    if (p[i]!=policy_pattern[i]) return 0;
  }
  return p[25]==0xe9 || !memcmp(p+25,policy_pattern+25,5);
}

/* Tie both gate sites to the same credential helper, and the first gate to
 * the access-manager call followed by the thread GS load. Reconstruct the
 * policy bypass from the native conditional branch, never a stored rel32. */
static int candidate(pbs_ps4_layout_read read, void *opaque, uint64_t base,
                      uint64_t text, size_t size, uint64_t a, pbs_ps4_firmware *out) {
  unsigned char buf[0x4000], allow[sizeof allow_pattern-1], stub[16], target_bytes[8];
  uint64_t auth, policy_auth, acmgr=0, bypass=0, policy=0;
  if (!inside(a,text,size,sizeof allow) || read(opaque,a,allow,sizeof allow) ||
      (allow[3]!=0x77 && allow[3]!=0xeb) || (int8_t)allow[4]!=28 ||
      relative(a+53,allow+49,&auth) || !inside(auth,text,size,1)) return -1;
  size_t before=(size_t)(a-text); if (before>0x400) before=0x400;
  if (read(opaque,a-before,buf,before)) return -1;
  unsigned acmgr_count=0;
  static const unsigned char gs[]={0x65,0x4c,0x8b,0x3c,0x25,0,0,0,0,0x49,0x8b,0x7f,0x08};
  for (size_t i=0;i+5+sizeof gs<=before;++i) {
    if (buf[i]!=0xe8 || memcmp(buf+i+5,gs,sizeof gs)) continue;
    uint64_t dest;
    if (relative(a-before+i+5,buf+i+1,&dest) || !inside(dest,text,size,sizeof stub) ||
        read(opaque,dest,stub,sizeof stub)) return -1;
    /* Jailbreak-provided allow stub, not a target to patch. */
    if (memcmp(stub,"\xb0\x01\xc3",3)) return -1;
    acmgr=dest; ++acmgr_count;
  }
  if (acmgr_count!=1) return -1;
  size_t after=size-(size_t)(a-text); if (after>sizeof buf) after=sizeof buf;
  if (read(opaque,a,buf,after)) return -1;
  unsigned policy_count=0;
  for (size_t i=sizeof allow;i+sizeof policy_pattern-1<=after;++i) {
    unsigned char *p=buf+i;
    if (!policy_match(p)) continue;
    if (relative(a+i+41,p+37,&policy_auth) || policy_auth!=auth ||
        relative(a+i+99,p+95,&bypass) || !inside(bypass,text,size,sizeof target_bytes) ||
        read(opaque,bypass,target_bytes,sizeof target_bytes) ||
        memcmp(target_bytes,"\x44\x89\xfa\xe9",4)) return -1;
    int64_t jump=(int64_t)(bypass-(a+i+25+5));
    if (jump<INT32_MIN || jump>INT32_MAX) return -1;
    int32_t encoded=(int32_t)jump;
    if (p[25]==0xe9 && memcmp(p+26,&encoded,4)) return -1;
    policy=a+i+25; ++policy_count;
  }
  if (policy_count!=1 || a<base || policy<base || acmgr<base ||
      policy-base>UINT_MAX || acmgr-base>UINT_MAX || a-base>UINT_MAX-3) return -1;
  memset(out,0,sizeof *out);
  out->name="Runtime-validated ptrace / exported Avcap ABI";
  out->acmgr_offset=(unsigned)(acmgr-base);
  memcpy(out->acmgr_signature,"\xb0\x01\xc3",3);
  out->gates[0].offset=(unsigned)(a+3-base);
  out->gates[0].size=2;
  memcpy(out->gates[0].before,allow+3,2); out->gates[0].before[0]=0x77;
  memcpy(out->gates[0].after,allow+3,2); out->gates[0].after[0]=0xeb;
  out->gates[1].offset=(unsigned)(policy-base); out->gates[1].size=5;
  memcpy(out->gates[1].before,policy_pattern+25,5);
  out->gates[1].after[0]=0xe9;
  int32_t delta=(int32_t)(bypass-(policy+5));
  memcpy(out->gates[1].after+1,&delta,4);
  return 0;
}

int pbs_ps4_layout_kernel(pbs_ps4_layout_read read, void *opaque,
                          uint64_t base, size_t image_size, pbs_ps4_firmware *out) {
  Elf64_Ehdr eh;
  Elf64_Phdr ph[32];
  if (!read || !out || !base || image_size<sizeof eh || image_size>0x4000000u ||
      base>UINT64_MAX-image_size || read(opaque,base,&eh,sizeof eh) ||
      memcmp(eh.e_ident,"\x7f" "ELF\x02\x01",6) || eh.e_machine!=EM_X86_64 ||
      eh.e_phentsize!=sizeof ph[0] || !eh.e_phnum || eh.e_phnum>32 ||
      eh.e_phoff>image_size || eh.e_phnum*sizeof ph[0]>image_size-eh.e_phoff ||
      read(opaque,base+eh.e_phoff,ph,eh.e_phnum*sizeof ph[0])) return -1;
  uint64_t linked=UINT64_MAX;
  for (unsigned i=0;i<eh.e_phnum;++i)
    if (ph[i].p_type==PT_LOAD && ph[i].p_vaddr<linked) linked=ph[i].p_vaddr;
  if (linked==UINT64_MAX) return -1;
  unsigned matches=0;
  pbs_ps4_firmware found;
  for (unsigned s=0;s<eh.e_phnum;++s) {
    if (ph[s].p_type!=PT_LOAD || !(ph[s].p_flags&PF_X)) continue;
    uint64_t off=ph[s].p_vaddr-linked;
    if (ph[s].p_vaddr<linked || off>=image_size || !ph[s].p_filesz ||
        ph[s].p_filesz>ph[s].p_memsz || ph[s].p_filesz>image_size-off) return -1;
    unsigned char block[4096+sizeof allow_pattern];
    for (size_t pos=0;pos<ph[s].p_filesz;) {
      size_t count=ph[s].p_filesz-pos; if (count>sizeof block) count=sizeof block;
      if (read(opaque,base+off+pos,block,count)) return -1;
      size_t step=count>4096?4096:count;
      for (size_t i=0;i<step && i+sizeof allow_pattern-1<=count;++i) {
        if (!match(block+i,allow_pattern,allow_mask)) continue;
        if (candidate(read,opaque,base,base+off,(size_t)ph[s].p_filesz,base+off+pos+i,&found))
          return -1;
        if (++matches>1) return -1;
      }
      pos+=step;
    }
  }
  if (matches!=1) return -1;
  *out=found;
  return 0;
}

intptr_t pbs_ps4_layout_trap(pbs_ps4_layout_read read, void *opaque, intptr_t anchor) {
  if (!read || anchor<0x10000 || anchor>INTPTR_MAX-0x10000) return 0;
  /* Scan around the named export, not an assumed module-relative location.
   * A run is twenty or more FF25 import stubs plus ten CC padding bytes.
   * Require exactly one run and preserve every import JMP. */
  uint64_t start=((uint64_t)anchor&~UINT64_C(0xfff))-0x10000;
  unsigned char block[4096+16];
  uint64_t previous=0, first=0, result=0;
  unsigned run=0, matches=0;
  for (size_t pos=0;pos<0x20000;pos+=4096) {
    if (read(opaque,start+pos,block,sizeof block)) { run=0; continue; }
    for (unsigned i=0;i<4096;++i) {
      uint64_t at=start+pos+i;
      if (block[i]!=0xff || block[i+1]!=0x25) continue;
      unsigned j=6; for (;j<16 && block[i+j]==0xcc;++j) {}
      if (j!=16) continue;
      uint64_t got, function;
      if (relative(at+6,block+i+2,&got) || got>INT64_MAX-sizeof function ||
          got<0x10000 || read(opaque,got,&function,sizeof function) ||
          function<0x10000 || function>INT64_MAX) { run=0; continue; }
      if (run && at==previous+16) ++run;
      else { first=at+6; run=1; }
      previous=at;
      if (run==PBS_PS4_CALL_SLOTS) { result=first; if (++matches>1) return 0; }
    }
  }
  return matches==1 ? (intptr_t)result : 0;
}
