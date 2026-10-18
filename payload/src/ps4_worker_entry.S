/* SPDX-License-Identifier: GPL-3.0-or-later
 * Position-independent normal SysV pthread entry. Copied to an owned mapping.
 * No breakpoints, signal handlers, TLS borrowing or ptrace in the RPC loop.
 * x86 TSO provides the acquire/release ordering paired with C11 host atomics.
 * On exit, tail-call native munmap and return into the native pthread caller,
 * so no instruction executes from the mapping after it has been unmapped. */
.section .rodata.pbs_worker,"a",@progbits
.global pbs_worker_code_start
.global pbs_worker_code_end
pbs_worker_code_start:
  push %rbx
  mov %rdi,%rbx
  movl $1,4(%rbx)
1:
  mov (%rbx),%eax
  cmp $1,%eax
  je 2f
  cmp $3,%eax
  je 3f
  mov $1000,%edi
  call *72(%rbx)
  jmp 1b
2:
  mov 16(%rbx),%rax
  mov 24(%rbx),%rdi
  mov 32(%rbx),%rsi
  mov 40(%rbx),%rdx
  mov 48(%rbx),%rcx
  mov 56(%rbx),%r8
  mov 64(%rbx),%r9
  call *%rax
  mov %rax,8(%rbx)
  movl $2,(%rbx)
  jmp 1b
3:
  mov 80(%rbx),%rax
  mov 88(%rbx),%rdi
  mov 96(%rbx),%rsi
  movl $4,(%rbx)
  pop %rbx
  jmp *%rax
pbs_worker_code_end:
.section .note.GNU-stack,"",@progbits
