// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/offset/profile.h"

const pbs_ps5_offset_profile pbs_ps5_offsets_320 = {
  .firmware = 0x03200004u,
  .avcap2 = {
    .open_video_rva = 0x360u,
    .ipc_command_instruction_rva = 0x3b45u,
    .request_size = 0x218u,
    .kind_offset = 0xe8u,
  },
  .remoteplay = {
    .request_size_store_rva = 0x1af429u,
    .rate = {
      .offset = 0x78u,
      .size = 0x68u,
      .descriptor_size = 0x20u,
    },
    .image = {
      .offset = 0x130u,
      .size = 0xe8u,
      .descriptor_size = 0x40u,
    },
    .width_offset = 0x150u,
    .height_offset = 0x154u,
    .paste_offset = 0x178u,
    .video_mode_offset = 0x200u,
    .mask_mode_offset = 0x204u,
    .mask_span = 0x14u,
  },
};
