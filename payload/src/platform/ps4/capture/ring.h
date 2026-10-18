// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/* FW11 libSceAvcap +0x1b60/+0x1c10/+0x1db0. All these structures live in
 * userspace. Do not substitute kernel dynlib structures for this ABI. */
#include <stdint.h>
#include <stddef.h>

typedef struct {
  uint64_t mapping;
  uint64_t mapping_size;
  int32_t read, pending;
  uint32_t generation;
} pbs_ps4_ring_cursor;

typedef struct {
  int32_t size, write, oldest;
  uint32_t generation;
} pbs_ps4_ring_header;

typedef struct {
  int32_t metadata_size, reservation, length, reserved;
} pbs_ps4_ring_entry;

_Static_assert(offsetof(pbs_ps4_ring_cursor, read) == 0x10, "FW11 read cursor");
_Static_assert(offsetof(pbs_ps4_ring_cursor, generation) == 0x18, "FW11 generation");
_Static_assert(sizeof(pbs_ps4_ring_header) == 16, "FW11 ring header");
_Static_assert(sizeof(pbs_ps4_ring_entry) == 16, "FW11 entry header");

static inline int pbs_ps4_ring_valid(const pbs_ps4_ring_cursor *c,
                                      const pbs_ps4_ring_header *h) {
  return c->mapping && c->mapping <= INT64_MAX - c->mapping_size &&
      c->mapping_size >= 32 && c->mapping_size <= 64u * 1024u * 1024u &&
      h->size >= 32 && (uint64_t)h->size <= c->mapping_size &&
      c->read >= 16 && c->read <= h->size &&
      h->write >= 16 && h->write <= h->size &&
      h->oldest >= 16 && h->oldest <= h->size;
}

/* Exact native overrun predicate (+0x1cd9..+0x1d29). Use wide intermediates
 * for the distance so malformed headers cannot overflow signed arithmetic. */
static inline int pbs_ps4_ring_lapped(const pbs_ps4_ring_header *h,
                                      int32_t read, uint32_t generation) {
  int64_t capacity = (int64_t)h->size - 16;
  int64_t distance = h->oldest;
  if (h->oldest < h->write) distance += capacity;
  if (h->write <= read || h->generation != generation) distance += capacity;
  distance -= read;
  return distance >= capacity || (int32_t)(h->generation - generation) >= 2;
}
