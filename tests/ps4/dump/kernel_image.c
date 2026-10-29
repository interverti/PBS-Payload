// SPDX-License-Identifier: GPL-3.0-or-later
#include "dump/ps4/kernel_image.h"
#include <assert.h>
#include <elf.h>
#include <stdio.h>
#include <string.h>

static unsigned char memory[0x7000];
static const uint64_t base=UINT64_C(0xffffffff80000000);
static int fail_code, reads;
static int read_image(void *opaque,uint64_t at,void *out,size_t size) {
  (void)opaque;
  assert(at>=base && size<=sizeof memory && at-base<=sizeof memory-size);
  assert(size<=4096);
  /* Neither the data segment nor the unmapped gap may be acquired. */
  assert(at-base<0x1000 || (at-base>=0x3000 && at-base+size<=0x5000));
  reads++;
  if(fail_code && at-base>=0x3000)return -1;
  memcpy(out,memory+(size_t)(at-base),size);return 0;
}
static Elf64_Phdr *setup(void) {
  memset(memory,0,sizeof memory);
  Elf64_Ehdr *eh=(void *)memory;
  memcpy(eh->e_ident,"\x7f" "ELF\x02\x01",6);
  eh->e_machine=EM_X86_64;eh->e_phoff=sizeof *eh;
  eh->e_phnum=3;eh->e_phentsize=sizeof(Elf64_Phdr);
  Elf64_Phdr *ph=(void *)(memory+eh->e_phoff);
  ph[0]=(Elf64_Phdr){.p_type=PT_LOAD,.p_flags=PF_R,.p_vaddr=0x100000,.p_filesz=0x1000,.p_memsz=0x1000};
  ph[1]=(Elf64_Phdr){.p_type=PT_LOAD,.p_flags=PF_R|PF_X,.p_offset=0x1000,.p_vaddr=0x103000,.p_filesz=0x2000,.p_memsz=0x2000};
  ph[2]=(Elf64_Phdr){.p_type=PT_LOAD,.p_flags=PF_R|PF_W,.p_vaddr=0x106000,.p_filesz=0x1000,.p_memsz=0x1000};
  memset(memory+0x3000,0xa5,0x2000);
  reads=0;fail_code=0;return ph;
}
int main(void) {
  setup();FILE *out=tmpfile(),*report=tmpfile();assert(out && report);
  assert(!pbs_ps4_dump_kernel_code(read_image,NULL,base,sizeof memory,out,report));
  assert(reads==4); /* Header, program table, two bounded code reads. */
  assert(ftell(out)==0x5000);
  assert(!fseek(out,0x3000,SEEK_SET));
  unsigned char code[0x2000];assert(fread(code,1,sizeof code,out)==sizeof code);
  assert(!memcmp(code,memory+0x3000,sizeof code));
  assert(!fseek(out,0x2000,SEEK_SET));assert(fgetc(out)==0);
  fclose(out);fclose(report);
  for(int scenario=0;scenario<4;scenario++) {
    Elf64_Phdr *ph=setup();out=tmpfile();report=tmpfile();assert(out && report);
    if(scenario==0)ph[1].p_filesz=sizeof memory;
    if(scenario==1)ph[1].p_flags=PF_R;
    if(scenario==2)fail_code=1;
    if(scenario==3)((Elf64_Ehdr *)memory)->e_phoff=sizeof memory-1;
    assert(pbs_ps4_dump_kernel_code(read_image,NULL,base,sizeof memory,out,report)<0);
    if(scenario!=2)assert(ftell(out)==0);
    fclose(out);fclose(report);
  }
  setup();out=fopen("/dev/full","wb");report=tmpfile();assert(out && report);
  assert(pbs_ps4_dump_kernel_code(read_image,NULL,base,sizeof memory,out,report)<0);
  fclose(out);fclose(report);
  puts("PS4 kernel dump: memory offsets, executable-only reads, bounds and I/O failures verified");
  return 0;
}
