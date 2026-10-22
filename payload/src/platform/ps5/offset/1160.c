// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset/profile.h"

const pbs_ps5_offset_profile pbs_ps5_offsets_1160 = {
  .firmware = 0x11600005u,
  .avcap2 = {
    .open_video_rva = 0x360u,
    .ipc_command_instruction_rva = 0x4487u,
    .request_size = 0x2e0u,
    .kind_offset = 0x1a0u,
  },
  .remoteplay = {
    .request_size_store_rva = 0x56fc8u,
    .rate = {
      .offset = 0xb8u,
      .size = 0xe0u,
      .descriptor_size = 0x20u,
    },
    .image = {
      .offset = 0x1e8u,
      .size = 0xf8u,
      .descriptor_size = 0x28u,
    },
    .width_offset = 0x208u,
    .height_offset = 0x20cu,
    .paste_offset = 0x218u,
    .video_mode_offset = 0x2a0u,
    .mask_mode_offset = 0x2a4u,
  },
};
