// SPDX-License-Identifier: GPL-3.0-or-later
#include "dump/ps4/kernel_image.h"
#include <elf.h>
#include <string.h>

int pbs_ps4_dump_kernel_code(pbs_ps4_layout_read read, void *opaque,
    uint64_t base, size_t image_size, FILE *image, FILE *report) {
  Elf64_Ehdr eh;
  Elf64_Phdr ph[32];
  if (!read || !image || !report || !base || image_size < sizeof eh ||
      image_size > 0x4000000u || base > UINT64_MAX-image_size ||
      read(opaque,base,&eh,sizeof eh) ||
      memcmp(eh.e_ident,"\x7f" "ELF\x02\x01",6) || eh.e_machine!=EM_X86_64 ||
      eh.e_phentsize!=sizeof ph[0] || !eh.e_phnum || eh.e_phnum>32 ||
      eh.e_phoff<sizeof eh || eh.e_phoff>image_size ||
      eh.e_phnum*sizeof ph[0]>image_size-eh.e_phoff ||
      read(opaque,base+eh.e_phoff,ph,eh.e_phnum*sizeof ph[0])) return -1;
  uint64_t linked=UINT64_MAX;
  unsigned executable=0;
  for(unsigned i=0;i<eh.e_phnum;i++)
    if(ph[i].p_type==PT_LOAD && ph[i].p_vaddr<linked)linked=ph[i].p_vaddr;
  if(linked==UINT64_MAX)return -1;
  /* Validate every range before acquiring any code. Mirror the runtime
   * scanner's memory-image mapping; p_offset is not a runtime address. */
  for(unsigned i=0;i<eh.e_phnum;i++) {
    if(ph[i].p_type!=PT_LOAD || !(ph[i].p_flags&PF_X))continue;
    uint64_t off=ph[i].p_vaddr-linked;
    if(ph[i].p_vaddr<linked || off>=image_size || !ph[i].p_filesz ||
       ph[i].p_filesz>ph[i].p_memsz || ph[i].p_filesz>image_size-off)return -1;
    executable++;
  }
  if(!executable)return -1;
  if(fwrite(&eh,1,sizeof eh,image)!=sizeof eh ||
     fseek(image,(long)eh.e_phoff,SEEK_SET) ||
     fwrite(ph,sizeof ph[0],eh.e_phnum,image)!=eh.e_phnum)return -1;
  unsigned char block[4096];
  for(unsigned i=0;i<eh.e_phnum;i++) {
    if(ph[i].p_type!=PT_LOAD || !(ph[i].p_flags&PF_X))continue;
    size_t off=(size_t)(ph[i].p_vaddr-linked),size=(size_t)ph[i].p_filesz;
    if(fseek(image,(long)off,SEEK_SET))return -1;
    for(size_t pos=0;pos<size;) {
      size_t count=size-pos;if(count>sizeof block)count=sizeof block;
      if(read(opaque,base+off+pos,block,count)) {
        fprintf(report,"kernel code read failed offset=0x%zx size=0x%zx\n",off+pos,count);
        return -1;
      }
      if(fwrite(block,1,count,image)!=count)return -1;
      pos+=count;
    }
    fprintf(report,"kernel code segment=%u offset=0x%zx size=0x%zx status=complete\n",i,off,size);
  }
  return fflush(image) || ferror(image) ? -1 : 0;
}
