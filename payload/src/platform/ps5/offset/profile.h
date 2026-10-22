// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

/* RVAs are relative to the named module. Request offsets start at Params. */
typedef struct {
  uint32_t offset;
  uint32_t size;
  uint32_t descriptor_size;
} pbs_ps5_request_group;

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
} pbs_ps5_remoteplay_offsets;

typedef struct pbs_ps5_offset_profile {
  uint32_t firmware;
  pbs_ps5_avcap2_offsets avcap2;
  pbs_ps5_remoteplay_offsets remoteplay;
} pbs_ps5_offset_profile;

const pbs_ps5_offset_profile *pbs_ps5_find_offsets(uint32_t firmware);
