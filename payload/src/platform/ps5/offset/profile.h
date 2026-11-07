// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* RVAs are relative to the named module. Request offsets start at Params. */
typedef struct {
  uint32_t offset;
  uint32_t size;
  uint32_t descriptor_size;
} pbs_ps5_request_group;

#define PBS_PS5_REQUEST_INITIALIZER_MAX_BYTES 16u

typedef struct {
  uint16_t offset;
  uint8_t size;
  uint8_t bytes[PBS_PS5_REQUEST_INITIALIZER_MAX_BYTES];
} pbs_ps5_request_initializer;

typedef struct {
  uint32_t open_video_rva;
  uint32_t ipc_command_instruction_rva;
  uint32_t request_size;
  uint32_t kind_offset;
} pbs_ps5_avcap2_offsets;

typedef struct {
  /* Diagnostic constructor location only; zero means not observed. */
  uint32_t request_size_store_rva;
  pbs_ps5_request_group rate;
  pbs_ps5_request_group image;
  uint32_t width_offset;
  uint32_t height_offset;
  uint32_t paste_offset;
  uint32_t video_mode_offset;
  uint32_t mask_mode_offset;
  /* Number of bytes after mask_mode_offset; zero means the default 0x24. */
  uint32_t mask_span;
  /* Constant constructor writes recovered from this firmware's decrypted
   * SceRemotePlay image. Dynamic fields remain caller-owned and are filled
   * explicitly after these initializers are applied. */
  const pbs_ps5_request_initializer *initializers;
  uint16_t initializer_count;
} pbs_ps5_remoteplay_offsets;

typedef struct pbs_ps5_offset_profile {
  /* Reference dump ID; a family fallback may run on another firmware. */
  uint32_t firmware;
  pbs_ps5_avcap2_offsets avcap2;
  pbs_ps5_remoteplay_offsets remoteplay;
} pbs_ps5_offset_profile;

static inline int pbs_ps5_apply_request_initializers(
    void *request, size_t request_size,
    const pbs_ps5_remoteplay_offsets *remoteplay) {
  if (!request || !remoteplay || !remoteplay->initializers ||
      !remoteplay->initializer_count) return -1;
  unsigned char *bytes = request;
  for (uint16_t i = 0; i < remoteplay->initializer_count; ++i) {
    const pbs_ps5_request_initializer *initializer =
        &remoteplay->initializers[i];
    if (!initializer->size ||
        initializer->size > PBS_PS5_REQUEST_INITIALIZER_MAX_BYTES ||
        initializer->offset > request_size ||
        initializer->size > request_size - initializer->offset) return -1;
    memcpy(bytes + initializer->offset, initializer->bytes,
           initializer->size);
  }
  return 0;
}

/* Prefer exact IDs, then the explicitly configured inclusive release ranges. */
const pbs_ps5_offset_profile *pbs_ps5_find_offsets(uint32_t firmware);
