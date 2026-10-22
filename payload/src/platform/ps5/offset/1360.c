// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset/profile.h"

const pbs_ps5_offset_profile pbs_ps5_offsets_1360 = {
  .firmware = 0x13600007u,
  .avcap2 = {
    .open_video_rva = 0x360u,
    .ipc_command_instruction_rva = 0x4507u,
    .request_size = 0x2e8u,
    .kind_offset = 0x1a8u,
  },
  .remoteplay = {
    .request_size_store_rva = 0x6f8d8u,
    .rate = {
      .offset = 0xb8u,
      .size = 0xe8u,
      .descriptor_size = 0x20u,
    },
    .image = {
      .offset = 0x1f0u,
      .size = 0xf8u,
      .descriptor_size = 0x28u,
    },
    .width_offset = 0x210u,
    .height_offset = 0x214u,
    .paste_offset = 0x220u,
    .video_mode_offset = 0x2a8u,
    .mask_mode_offset = 0x2acu,
  },
};
