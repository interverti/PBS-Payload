// SPDX-License-Identifier: GPL-3.0-or-later
/* PBS - PS5 Broadcast Stream: remote calls inside another process. */

#include "inject.h"

#include <errno.h>
#include <machine/reg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/mdbg.h>

/* Credentials required for ptrace calls against a system process. */
#define PBS_PTRACE_AUTHID 0x4800000000010003L

static int pb_ptrace(int request, pid_t pid, caddr_t addr, int data) {
  pid_t me = getpid();
  uint8_t privcaps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  uint8_t caps[16];
  uint64_t authid = kernel_get_ucred_authid(me);
  if (!authid || kernel_get_ucred_caps(me, caps)) return -1;
  if (kernel_set_ucred_authid(me, PBS_PTRACE_AUTHID)) return -1;
  if (kernel_set_ucred_caps(me, privcaps)) {
    kernel_set_ucred_authid(me, authid);
    return -1;
  }

  int ret = (int)syscall(SYS_ptrace, request, pid, addr, data);

  int restore = kernel_set_ucred_caps(me, caps) |
                kernel_set_ucred_authid(me, authid);
  if (restore) return -1;
  return ret;
}

pid_t pbs_find_pid(const char *name) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0)) return -1;
  void *buf = malloc(size);
  if (!buf) return -1;
  if (sysctl(mib, 4, buf, &size, NULL, 0)) {
    free(buf);
    return -1;
  }
  pid_t found = -1;
  for (unsigned char *p = buf; p < (unsigned char *)buf + size;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)p;
    p += ki->ki_structsize;
    if (!strcmp(ki->ki_comm, name)) {
      found = ki->ki_pid;
      break;
    }
  }
  free(buf);
  return found;
}

intptr_t pbs_resolve_in(pid_t pid, const char *module, const char *symbol) {
  uint32_t handle = 0;
  if (kernel_dynlib_handle(pid, module, &handle)) return 0;
  return kernel_dynlib_dlsym(pid, handle, symbol);
}

/* Wait for a process attached with PT_ATTACH to stop; report the stop signal. */
static int pb_wait_stop_sig(pid_t pid, int timeout_ms, int *sig) {
  if (sig) *sig = 0;
  for (int waited = 0; waited < timeout_ms; waited += 5) {
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG | WUNTRACED);
    if (r == pid) {
      if (sig && WIFSTOPPED(status)) *sig = WSTOPSIG(status);
      return WIFSTOPPED(status) ? 0 : -1;
    }
    if (r < 0 && errno != EINTR) return -1;
    usleep(5 * 1000);
  }
  return -1;
}

static int pb_wait_stop(pid_t pid, int timeout_ms) {
  return pb_wait_stop_sig(pid, timeout_ms, NULL);
}

static int pb_read(pid_t pid, intptr_t addr, void *out, size_t len) {
  struct ptrace_io_desc iod = {
      .piod_op = PIOD_READ_D,
      .piod_offs = (void *)addr,
      .piod_addr = out,
      .piod_len = len,
  };
  return pb_ptrace(PT_IO, pid, (caddr_t)&iod, 0);
}

static int pb_write(pid_t pid, intptr_t addr, const void *in, size_t len) {
  struct ptrace_io_desc iod = {
      .piod_op = PIOD_WRITE_D,
      .piod_offs = (void *)addr,
      .piod_addr = (void *)in,
      .piod_len = len,
  };
  return pb_ptrace(PT_IO, pid, (caddr_t)&iod, 0);
}

int pbs_inject_begin(const char *proc_name, pbs_inject_t *ctx) {
  memset(ctx, 0, sizeof *ctx);
  ctx->pid = pbs_find_pid(proc_name);
  if (ctx->pid <= 0) return -1;

  if (pb_ptrace(PT_ATTACH, ctx->pid, 0, 0)) return -1;
  ctx->attached = 1;
  if (pb_wait_stop(ctx->pid, 2000)) {
    pbs_inject_end(ctx);
    return -1;
  }

  /* Reserve writable scratch below the stopped thread stack pointer. The stack grows downwards. */
  struct reg regs;
  if (pb_ptrace(PT_GETREGS, ctx->pid, (caddr_t)&regs, 0)) {
    pbs_inject_end(ctx);
    return -1;
  }
  ctx->scratch_len = 0x1000;
  ctx->scratch = (intptr_t)((regs.r_rsp - 0x4000) & ~UINT64_C(15));
  if (ctx->scratch <= 0) {
    pbs_inject_end(ctx);
    return -1;
  }
  /* Verify that the scratch page is writable before proceeding. */
  int prot = kernel_get_vmem_protection(ctx->pid, ctx->scratch, 16);
  if (prot < 0 || !(prot & PROT_WRITE)) {
    pbs_inject_end(ctx);
    return -1;
  }
  return 0;
}

long pbs_inject_call(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]) {
  struct reg bak, jmp;
  if (pb_ptrace(PT_GETREGS, ctx->pid, (caddr_t)&bak, 0)) return LONG_MIN;
  memcpy(&jmp, &bak, sizeof jmp);
  jmp.r_rip = fn;
  /* SysV call frame: align RSP to 16 bytes and leave 0x200 bytes for the callee. */
  jmp.r_rsp = (bak.r_rsp - 0x200) & ~UINT64_C(15);
  jmp.r_rdi = args[0];
  jmp.r_rsi = args[1];
  jmp.r_rdx = args[2];
  jmp.r_rcx = args[3];
  jmp.r_r8 = args[4];
  jmp.r_r9 = args[5];
  if (pb_ptrace(PT_SETREGS, ctx->pid, (caddr_t)&jmp, 0)) return LONG_MIN;

  /* Single-step until RSP passes the initial value, indicating a return. Resume assertion stops with SIGCONT and record the assertion. */
  uint64_t start_rsp = (uint64_t)jmp.r_rsp;
  int steps = 0;
  long result = LONG_MIN;
  while (steps++ < 20000) {
    if (pb_ptrace(PT_STEP, ctx->pid, (caddr_t)1, 0)) break;
    int sig = 0;
    if (pb_wait_stop_sig(ctx->pid, 500, &sig)) break;
    if (sig == SIGSTOP) {
      kill(ctx->pid, SIGCONT);
      ctx->asserted = 1;
      break;
    }
    if (pb_ptrace(PT_GETREGS, ctx->pid, (caddr_t)&jmp, 0)) break;
    if ((uint64_t)jmp.r_rsp > start_rsp) {
      result = (long)jmp.r_rax;
      break;
    }
  }
  pb_ptrace(PT_SETREGS, ctx->pid, (caddr_t)&bak, 0);
  return result;
}

int pbs_inject_read_at(pbs_inject_t *ctx, intptr_t addr, void *out, size_t len) {
  return pb_read(ctx->pid, addr, out, len);
}

int pbs_inject_write_at(pbs_inject_t *ctx, intptr_t addr, const void *in, size_t len) {
  return pb_write(ctx->pid, addr, in, len);
}

/* FreeBSD amd64 layout: environment (8 bytes), x87 (80 bytes), XMM0-15 (16 bytes each). */
typedef struct {
  unsigned short env[4];
  unsigned char acc[8][10];
  unsigned char xacc[16][16];
  unsigned short spare[12];
} pbs_fpreg_t;

static long pb_call_fast(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6], const float *xmm0) {
  struct reg bak, regs;
  intptr_t trap_addr = ctx->scratch + 0xf00;
  intptr_t page = trap_addr & ~(intptr_t)0x3fff;
  int prot = kernel_get_vmem_protection(ctx->pid, page, 0x4000);
  unsigned char original, trap = 0xcc;
  uint64_t stack_word;
  pbs_fpreg_t fpbak, fpnew;
  int fp_changed = 0;
  if (!fn || prot < 0 || pb_ptrace(PT_GETREGS, ctx->pid, (caddr_t)&bak, 0) ||
      pb_read(ctx->pid, trap_addr, &original, 1)) return LONG_MIN;
  if (xmm0) {
    if (pb_ptrace(PT_GETFPREGS, ctx->pid, (caddr_t)&fpbak, 0)) return LONG_MIN;
    memcpy(&fpnew, &fpbak, sizeof fpnew);
    memcpy(&fpnew.xacc[0][0], xmm0, sizeof *xmm0);
  }
  regs = bak;
  regs.r_rsp = ((bak.r_rsp - 0x200) & ~(intptr_t)15) - 8;
  if (pb_read(ctx->pid, regs.r_rsp, &stack_word, sizeof stack_word)) return LONG_MIN;
  if (kernel_set_vmem_protection(ctx->pid, page, 0x4000, prot | PROT_EXEC)) return LONG_MIN;
  long result = LONG_MIN;
  int changed_regs = 0, running = 0, sig = 0;
  if (pb_write(ctx->pid, trap_addr, &trap, 1) ||
      pb_write(ctx->pid, regs.r_rsp, &trap_addr, sizeof trap_addr)) goto fast_cleanup;
  regs.r_rip = fn;
  regs.r_rdi = args[0]; regs.r_rsi = args[1]; regs.r_rdx = args[2];
  regs.r_rcx = args[3]; regs.r_r8 = args[4]; regs.r_r9 = args[5];
  if (pb_ptrace(PT_SETREGS, ctx->pid, (caddr_t)&regs, 0)) goto fast_cleanup;
  changed_regs = 1;
  if (xmm0) {
    if (pb_ptrace(PT_SETFPREGS, ctx->pid, (caddr_t)&fpnew, 0)) goto fast_cleanup;
    fp_changed = 1;
  }
  if (pb_ptrace(PT_CONTINUE, ctx->pid, (caddr_t)1, 0)) goto fast_cleanup;
  running = 1;
  if (!pb_wait_stop_sig(ctx->pid, 12000, &sig)) {
    running = 0;
    if (!pb_ptrace(PT_GETREGS, ctx->pid, (caddr_t)&regs, 0) &&
        sig == SIGTRAP && regs.r_rip == trap_addr + 1) result = (long)regs.r_rax;
    if (sig == SIGSTOP) ctx->asserted = 1;
  }
fast_cleanup:
  if (running) {
    kill(ctx->pid, SIGSTOP);
    if (pb_wait_stop(ctx->pid, 2000)) return LONG_MIN;
  }
  int restore = pb_write(ctx->pid, trap_addr, &original, 1);
  intptr_t call_rsp = ((bak.r_rsp - 0x200) & ~(intptr_t)15) - 8;
  restore |= pb_write(ctx->pid, call_rsp, &stack_word, sizeof stack_word);
  if (changed_regs) restore |= pb_ptrace(PT_SETREGS, ctx->pid, (caddr_t)&bak, 0);
  if (fp_changed) restore |= pb_ptrace(PT_SETFPREGS, ctx->pid, (caddr_t)&fpbak, 0);
  restore |= kernel_set_vmem_protection(ctx->pid, page, 0x4000, prot);
  return restore ? LONG_MIN : result;
}

long pbs_inject_call_fast(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6]) {
  return pb_call_fast(ctx, fn, args, NULL);
}

long pbs_inject_call_fast_fp(pbs_inject_t *ctx, intptr_t fn, const uint64_t args[6], float xmm0) {
  return pb_call_fast(ctx, fn, args, &xmm0);
}

int pbs_inject_read(pbs_inject_t *ctx, void *out, size_t len) {
  if (len > ctx->scratch_len) len = ctx->scratch_len;
  return pb_read(ctx->pid, ctx->scratch, out, len);
}

int pbs_inject_write(pbs_inject_t *ctx, const void *in, size_t len) {
  if (len > ctx->scratch_len) len = ctx->scratch_len;
  return pb_write(ctx->pid, ctx->scratch, in, len);
}

void pbs_inject_end(pbs_inject_t *ctx) {
  if (ctx->attached) {
    pb_ptrace(PT_DETACH, ctx->pid, 0, 0);
    ctx->attached = 0;
  }
}
