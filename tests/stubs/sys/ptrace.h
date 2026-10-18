// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <sys/types.h>
#define PT_ATTACH 10
#define PT_DETACH 11
#define PT_IO 12
#define PIOD_READ_D 1
#define PIOD_WRITE_D 2
struct ptrace_io_desc {
  int piod_op;
  void *piod_offs, *piod_addr;
  size_t piod_len;
};
int ptrace(int request, pid_t pid, caddr_t addr, int data);
