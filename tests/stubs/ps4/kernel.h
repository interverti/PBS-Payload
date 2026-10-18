// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#ifndef PBS_TEST_FW_VERSION
#define PBS_TEST_FW_VERSION 0x11008001u
#endif
static inline uint32_t kernel_get_fw_version(void) { return PBS_TEST_FW_VERSION; }
#ifdef PBS_TEST_CONTROL
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#define KERNEL_ADDRESS_IMAGE_BASE ((intptr_t)0x10000000)
int kernel_copyout(intptr_t address, void *data, size_t size);
int kernel_copyin(const void *data, intptr_t address, size_t size);
typedef struct dynlib_obj {
  uintptr_t next;
  uintptr_t path;
  uint64_t unknown0[2];
  uint32_t refcount;
  uint64_t handle;
  uintptr_t mapbase;
  uintptr_t mapsize;
  uintptr_t textsize;
  uintptr_t database;
  size_t datasize;
} dynlib_obj_t;
int kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle);
int kernel_dynlib_obj(pid_t pid, uint32_t handle, dynlib_obj_t *obj);
#endif
