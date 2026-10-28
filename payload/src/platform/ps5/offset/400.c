// SPDX-License-Identifier: GPL-3.0-or-later
// EXPERIMENTAL: firmware 4.00 dump has a one-byte RIP/rodata mismatch.
// Disabled in normal payload builds: compile with -DPBS_PS5_EXPERIMENTAL_FW400.
#include "platform/ps5/offset/profile.h"

const pbs_ps5_offset_profile pbs_ps5_offsets_400 = {
  .firmware = 0x04000042u,
  .avcap2 = {
    .open_video_rva = 0x33fu,
    .ipc_command_instruction_rva = 0x3ac4u,
    .request_size = 0x230u,
    .kind_offset = 0x100u,
  },
  .remoteplay = {
    .request_size_store_rva = 0x1b7f12u,
    .rate = {
      .offset = 0x90u,
      .size = 0x68u,
      .descriptor_size = 0x20u,
    },
    .image = {
      .offset = 0x148u,
      .size = 0xe8u,
      .descriptor_size = 0x40u,
    },
    .width_offset = 0x168u,
    .height_offset = 0x16cu,
    .paste_offset = 0x190u,
    .video_mode_offset = 0x218u,
    .mask_mode_offset = 0x21cu,
    .mask_span = 0x14u,
  },
};
