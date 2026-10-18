// SPDX-License-Identifier: GPL-3.0-or-later
/* Execute the actual FW11 padding trampoline on x86-64. Its callee destroys
 * x87/MXCSR/all YMM registers and returns a 64-bit value. Test both the native
 * XSAVE path and a forced FXSAVE fallback, including all six integer args. */
#include "platform/ps4/system/trampoline.h"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <cpuid.h>

uint64_t received[6];
void *test_entry;
static const uint32_t changed_mxcsr __attribute__((used)) = 0x3f80;
extern uint64_t invoke(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
extern void destructive_callee(void);
__asm__(
".text\n"
"invoke:\n"
"push %rbx\n push %r12\n push %r13\n push %r14\n push %r15\n"
"call *test_entry(%rip)\n"
"pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbx\n ret\n"
"destructive_callee:\n"
"mov %rdi,received(%rip)\n mov %rsi,received+8(%rip)\n"
"mov %rdx,received+16(%rip)\n mov %rcx,received+24(%rip)\n"
"mov %r8,received+32(%rip)\n mov %r9,received+40(%rip)\n"
"vzeroall\n fldz\n ldmxcsr changed_mxcsr(%rip)\n"
"movabs $0x1234567887654321,%rax\n ret\n");

static void check(int fallback) {
  unsigned char *memory = mmap(NULL, 8192, PROT_READ|PROT_WRITE|PROT_EXEC,
                              MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  assert(memory != MAP_FAILED);
  unsigned char code[PBS_PS4_CALL_SLOTS][10];
  intptr_t base = (intptr_t)memory;
  assert(!pbs_ps4_call_code(code, base, base + 4096, base + 6144));
  memset(memory, 0xcc, 4096);
  memset(memory + 4096, 0, 1024);
  void (*fn)(void) = destructive_callee;
  memcpy(memory + 6144, &fn, sizeof fn);
  for (unsigned i=0; i<PBS_PS4_CALL_SLOTS; ++i) memcpy(memory+i*16,code[i],10);
  if (fallback) { /* Force CPUID carry branch, preserving normal slot layout. */
    memory[3*16] = 0xe9;
    assert(!pbs_ps4_call_disp(memory+3*16+1,base+9*16,base+3*16+5));
    memory[3*16+5] = 0x90;
  }
  /* Replace only INT3 with a host return. The debugger restores RSP on PS4. */
  const unsigned char host_return[] = {0x48,0x83,0xc4,8,0xc3};
  memcpy(memory+PBS_PS4_CALL_TRAP_DELTA,host_return,sizeof host_return);
  test_entry = memory;
  unsigned char before[512] __attribute__((aligned(64)));
  unsigned char after[512] __attribute__((aligned(64)));
  unsigned char ymm[32] __attribute__((aligned(32)));
  uint64_t result;
  /* Seed x87 and SIMD inside the same assembly statement as the call so no
   * compiler-generated floating point instructions can affect the oracle. */
  __asm__ volatile(
      "fninit\n fld1\n vpcmpeqd %%ymm0,%%ymm0,%%ymm0\n"
      "vmovdqa %%ymm0,%%ymm15\n fxsave64 %0\n"
      "mov $1,%%rdi\n mov $2,%%rsi\n mov $3,%%rdx\n"
      "mov $4,%%rcx\n mov $5,%%r8\n mov $6,%%r9\n call invoke\n"
      "fxsave64 %1\n vmovdqa %%ymm15,%2\n"
      : "=m"(before), "=m"(after), "=m"(ymm), "=a"(result)
      : : "rdi","rsi","rdx","rcx","r8","r9","r10","r11",
          "xmm0","xmm15","cc","memory");
  assert(result == UINT64_C(0x1234567887654321));
  for (unsigned i=0;i<6;++i) assert(received[i] == i+1);
  /* FXSAVE's reserved bytes are not architectural state. */
  assert(!memcmp(before,after,416));
  for (unsigned i=0;i<PBS_PS4_CALL_SLOTS-1;++i)
    for (unsigned j=10;j<16;++j) assert(memory[i*16+j]==0xcc);
  if (!fallback) {
    for (unsigned i=0;i<32;++i) assert(ymm[i]==0xff);
  }
  assert(!munmap(memory,8192));
}

int main(void) {
  unsigned a,b,c,d;
  assert(__get_cpuid(1,&a,&b,&c,&d) && (c & bit_AVX) && (c & bit_OSXSAVE));
  check(0);
  check(1);
  unsigned char code[PBS_PS4_CALL_SLOTS][10];
  assert(pbs_ps4_call_code(code,0x1000,0x1001,0x2000)<0);
  assert(pbs_ps4_call_code(code,0x1000,INT64_C(0x100000000),0x2000)<0);
  puts("FW11 trampoline: six args, 64-bit return, x87/SSE/MXCSR/AVX restore, FXSAVE fallback, import stubs unchanged");
}
