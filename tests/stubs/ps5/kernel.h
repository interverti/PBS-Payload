#pragma once
#include <stdint.h>
#include <sys/types.h>

int kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle);
intptr_t kernel_dynlib_mapbase_addr(pid_t pid, uint32_t handle);
intptr_t kernel_dynlib_dlsym(pid_t pid, uint32_t handle, const char *symbol);
uint32_t kernel_get_fw_version(void);
