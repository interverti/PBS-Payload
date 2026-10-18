// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef PBS_PS4
#include <sys/mman.h>
#endif

/* PS4 loader processes have a small libc heap. Large video buffers use
 * anonymous mappings; control structures and audio retain their normal heap.
 * Geometric capacity avoids reallocating on every slightly larger frame. */
static inline size_t pbs_video_capacity(size_t size) {
  size_t capacity = 0x4000;
  while (capacity < size && capacity <= SIZE_MAX / 2) capacity *= 2;
  return capacity >= size ? capacity : size;
}
static inline void *pbs_video_alloc(size_t size) {
#ifdef PBS_PS4
  void *p = mmap(NULL, pbs_video_capacity(size), PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0);
  return p == MAP_FAILED ? NULL : p;
#else
  return malloc(pbs_video_capacity(size));
#endif
}
static inline void pbs_video_free(void *buffer, size_t capacity) {
  if (!buffer) return;
#ifdef PBS_PS4
  munmap(buffer, pbs_video_capacity(capacity));
#else
  (void)capacity;
  free(buffer);
#endif
}
/* Contents need not survive growth: the caller overwrites the entire frame. */
static inline void *pbs_video_grow(void *buffer, size_t capacity, size_t size) {
  if (size <= capacity) return buffer;
#ifdef PBS_PS4
  void *next = pbs_video_alloc(size);
  if (next) pbs_video_free(buffer, capacity);
  return next;
#else
  return realloc(buffer, pbs_video_capacity(size));
#endif
}
