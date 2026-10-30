// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset/profile.h"

const pbs_ps5_offset_profile pbs_ps5_offsets_510 = {
  .firmware = 0x05100023u,
  .avcap2 = {
    .open_video_rva = 0x350u,
    .ipc_command_instruction_rva = 0x3d35u,
    .request_size = 0x298u,
    .kind_offset = 0x168u,
  },
  .remoteplay = {
    .request_size_store_rva = 0x472adu,
    .rate = {
      .offset = 0x90u,
      .size = 0xd0u,
      .descriptor_size = 0x20u,
    },
    .image = {
      .offset = 0x1b0u,
      .size = 0xe8u,
      .descriptor_size = 0x40u,
    },
    .width_offset = 0x1d0u,
    .height_offset = 0x1d4u,
    .paste_offset = 0x1f8u,
    .video_mode_offset = 0x280u,
    .mask_mode_offset = 0x284u,
    .mask_span = 0x14u,
  },
};
