// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset/profile.h"

const pbs_ps5_offset_profile pbs_ps5_offsets_1060 = {
  .firmware = 0x10600006u,
  .avcap2 = {
    .open_video_rva = 0x360u,
    .ipc_command_instruction_rva = 0x4227u,
    .request_size = 0x2b8u,
    .kind_offset = 0x188u,
  },
  .remoteplay = {
    .request_size_store_rva = 0x4d5adu,
    .rate = {
      .offset = 0xa8u,
      .size = 0xd8u,
      .descriptor_size = 0x20u,
    },
    .image = {
      .offset = 0x1d0u,
      .size = 0xe8u,
      .descriptor_size = 0x28u,
    },
    .width_offset = 0x1f0u,
    .height_offset = 0x1f4u,
    .paste_offset = 0x200u,
    .video_mode_offset = 0x288u,
    .mask_mode_offset = 0x28cu,
  },
};
