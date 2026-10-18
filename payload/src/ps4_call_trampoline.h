// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/* FW11 libSceAvcap import padding: ten bytes every sixteen bytes, starting
 * at +0xbe. Import JMP instructions between slots MUST remain unchanged.
 * Save/restore Jaguar x87/SSE/AVX state inside the tracee, without Orbis FP
 * ptrace. CPUID.OSXSAVE selects XSAVE/XRSTOR; otherwise use FXSAVE/FXRSTOR.
 * The caller zeroes a 64-byte-aligned 1024-byte save area before execution.
 * Integer SysV arguments survive CPUID, stack is aligned before CALL, and
 * RAX is preserved across XRSTOR. GPRs are restored by the debugger at INT3.
 * This does not make borrowing an arbitrary thread/TLS context safe. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>

#define PBS_PS4_CALL_SLOTS 20u
#define PBS_PS4_CALL_STATE_OFF 0x800u
#define PBS_PS4_CALL_FN_OFF 0xc00u
#define PBS_PS4_CALL_STATE_SIZE 1024u
#define PBS_PS4_CALL_TRAP_DELTA (19u * 16u + 3u)

static inline int pbs_ps4_call_disp(unsigned char *out, intptr_t target,
                                  intptr_t next) {
  int64_t delta = (int64_t)target - (int64_t)next;
  if (delta < INT32_MIN || delta > INT32_MAX) return -1;
  int32_t relative = (int32_t)delta;
  memcpy(out, &relative, sizeof relative);
  return 0;
}

static inline int pbs_ps4_call_code(unsigned char out[PBS_PS4_CALL_SLOTS][10],
                                  intptr_t base, intptr_t state, intptr_t fn) {
  static const unsigned char body[PBS_PS4_CALL_SLOTS][8] = {
    {0x48,0x83,0xec,8,0x49,0x89,0xd4}, /* sub rsp,8; mov r12,rdx */
    {0x49,0x89,0xcd,0xb8,1,0,0,0},    /* mov r13,rcx; mov eax,1 */
    {0x0f,0xa2,0x0f,0xba,0xe1,27},    /* cpuid; bt ecx,27 */
    {0x0f,0x83},                       /* jnc fxsave */
    {0x31,0xc9,0x0f,0x01,0xd0},       /* xor ecx,ecx; xgetbv */
    {0x83,0xe0,7,0x41,0x89,0xc6,0x31,0xd2}, /* mask; r14d=eax; edx=0 */
    {0x48,0x0f,0xae,0x25},            /* xsave64 [rip+state] */
    {0xbb,1,0,0,0},                    /* mov ebx,1 */
    {0xe9},                            /* jmp invoke */
    {0x48,0x0f,0xae,0x05},            /* fxsave64 [rip+state] */
    {0x31,0xdb},                       /* xor ebx,ebx */
    {0x4c,0x89,0xe2,0x4c,0x89,0xe9}, /* restore rdx,rcx */
    {0xff,0x15},                       /* call [rip+function] */
    {0x49,0x89,0xc7,0x85,0xdb},       /* r15=rax; test ebx,ebx */
    {0x0f,0x84},                       /* jz fxrstor */
    {0x44,0x89,0xf0,0x31,0xd2},       /* eax=r14d; edx=0 */
    {0x48,0x0f,0xae,0x2d},            /* xrstor64 [rip+state] */
    {0xe9},                            /* jmp finish */
    {0x48,0x0f,0xae,0x0d},            /* fxrstor64 [rip+state] */
    {0x4c,0x89,0xf8,0xcc}             /* rax=r15; int3 */
  };
  static const unsigned char lengths[] =
      {7,8,6,6,5,8,8,5,5,8,2,6,6,5,6,5,8,5,8,4};
  if (!out || base <= 0 || state <= 0 || (state & 63) || fn <= 0 ||
      base > INTPTR_MAX - PBS_PS4_CALL_SLOTS * 16u) return -1;
  for (unsigned i = 0; i < PBS_PS4_CALL_SLOTS; ++i) {
    memset(out[i], 0x90, 10);
    memcpy(out[i], body[i], lengths[i]);
    out[i][8] = 0xeb; out[i][9] = 6;
  }
  const unsigned state_slots[] = {6,9,16,18};
  for (unsigned i = 0; i < 4; ++i) {
    unsigned s = state_slots[i];
    if (pbs_ps4_call_disp(out[s] + 4, state, base + s * 16 + 8)) return -1;
  }
  if (pbs_ps4_call_disp(out[12] + 2, fn, base + 12 * 16 + 6)) return -1;
  const unsigned branches[][3] = {{3,9,2},{8,11,1},{14,18,2},{17,19,1}};
  for (unsigned i = 0; i < 4; ++i) {
    unsigned s = branches[i][0], offset = branches[i][2];
    if (pbs_ps4_call_disp(out[s] + offset, base + branches[i][1] * 16,
                         base + s * 16 + offset + 4)) return -1;
  }
  return 0;
}
