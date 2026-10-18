// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/* Observed 0x108-byte SceRemotePlay OpenVideoEx request on PS4 FW 11.00.
 * All offsets are bytes into the native 32-bit little-endian request. */
#include <stdint.h>
#include <string.h>

#ifndef PBS_PS4_NATIVE_SOURCE
#define PBS_PS4_NATIVE_SOURCE 0
#endif

#define PBS_PS4_VIDEO_REQUEST_SIZE 0x108u
#define PBS_PS4_VIDEO_REQUEST_WORDS (PBS_PS4_VIDEO_REQUEST_SIZE / sizeof(uint32_t))
#define PBS_PS4_DEFAULT_BITRATE_KBPS 10000u
#ifndef PBS_PS4_MEMORY_COUNT
#define PBS_PS4_MEMORY_COUNT 3
#endif
#if PBS_PS4_MEMORY_COUNT < 3 || PBS_PS4_MEMORY_COUNT > 5
#error Low-memory experiments require memory count 3..5
#endif

/* PBS's browser/muxer uses an H.264 90kHz track with 1500 ticks per
 * 60fps sample, matching the working PS5 backend. Avoid using poll-loop
 * wall clock jitter as an encoder timestamp while the PS4 metadata PTS ABI
 * is still being investigated. The counter survives an Avcap reopen. */
static inline uint32_t pbs_ps4_pts90k(uint64_t video_frame_index) {
  return (uint32_t)(video_frame_index * UINT64_C(1500));
}

static inline int pbs_ps4_make_video_request_at_bitrate(
    uint32_t request[PBS_PS4_VIDEO_REQUEST_WORDS], unsigned height,
    unsigned usage, unsigned native_image_paste, unsigned bitrate_kbps) {
  /* Application-side bounds, not a claim that every firmware/model supports
   * every bitrate/resolution combination. Native readiness still decides. */
  if (!request || (height != 720 && height != 1080) || usage > 1 ||
      native_image_paste > 1 || bitrate_kbps < 1000 || bitrate_kbps > 15000)
    return -1;
  memset(request, 0, PBS_PS4_VIDEO_REQUEST_SIZE);
  request[0x04 / 4] = usage;              /* Native RemotePlay: usage 0. */
  request[0x08 / 4] = request[0x0c / 4] = height == 720 ? 19 : 20;
  const uint32_t bitrate = bitrate_kbps * 1000u;
  /* Native RemotePlay uses DIVSS by float32 1.1, then truncates. */
  request[0x10 / 4] = (uint32_t)((float)bitrate / 1.1f);
  request[0x14 / 4] = bitrate;
  request[0x18 / 4] = 0x44;
  request[0x1c / 4] = 60;
  request[0x24 / 4] = 1;
  request[0x30 / 4] = 9;
  request[0x34 / 4] = bitrate / 8 / 60;
  request[0x38 / 4] = 35;
  request[0x3c / 4] = 51;
  /* +0x2fa675: EAX=EDI, loaded from session+0x198 (bitrate in kbps),
   * NOT ESI (fps). MULSD/ADDSD constants are at 0x4291b8..0x4291d0.
   * The old 1145/567 values incorrectly substituted 60 for 10000. */
  request[0x40 / 4] = (uint32_t)((double)bitrate_kbps * 0.262 + 1129.9);
  request[0x44 / 4] = (uint32_t)((double)bitrate_kbps * 0.2103 + 554.98);
  request[0x48 / 4] = bitrate / 8 / 60;
  /* Separate native encoder field from +0x08/+0x0c profile IDs.
   * RemotePlay +0x2fa632 sets 25 for 720p and +0x2fa654 sets 20 when
   * output width >=1920 and height >=1080. */
  request[0x4c / 4] = height == 720 ? 25 : 20;
  request[0x50 / 4] = bitrate / 60;
  request[0x54 / 4] = 60;
  /* Native RemotePlay uses a named ImagePaste overlay descriptor before
   * OpenVideoEx (+0x2fa72e..+0x2fa78d). SceAvCapture calls shm_open(name,
   * O_RDONLY) when nonempty; it fails the Open if the segment does not exist.
   * PBS does not own that memory, so the descriptor is OPT-IN and defaults
   * absent. No global display settings/shared memory are modified here. */
  if (native_image_paste) {
    request[0x5c / 4] = 0x01800000;
    request[0x60 / 4] = 0x01600180;
    request[0x64 / 4] = 0x00034000;
    static const char image_paste[32] = "/SceRemotePlayImagePaste";
    memcpy((unsigned char *)request + 0x68, image_paste, sizeof image_paste);
  }
  /* +0x88..+0x107 contains eight float32 privacy rectangles (x0,y0,x1,y1),
   * NOT reserved bytes. Native RP initially fills them with NaNs (0xff) and
   * later supplies its live rectangle list with SetInfo(9). FW11's SSE
   * clamping turns those NaNs into [0,0,1,1], masking the entire frame.
   * PBS owns no extra privacy rectangles: leave eight degenerate zero-area
   * rectangles from memset above. Service/display capture policy remains
   * under the firmware's control. See docs/PS4_FW11_PRIVACY_MASK.md. */
  return 0;
}

/* Isolated generic-profile experiment, reconstructed from FW11:
 * profile 2 is 1920x1080 at 30fps; its source-mode byte accepts 0xff as
 * a wildcard. It is not the RemotePlay 1080p60 profile 20. */
static inline int pbs_ps4_make_video_request_at_rate(
    uint32_t request[PBS_PS4_VIDEO_REQUEST_WORDS], unsigned height,
    unsigned usage, unsigned native_image_paste, unsigned fps) {
  if (fps != 60 && (fps != 30 || height != 1080 || usage || native_image_paste))
    return -1;
  if (pbs_ps4_make_video_request_at_bitrate(request, height, usage,
      native_image_paste, PBS_PS4_DEFAULT_BITRATE_KBPS)) return -1;
  if (fps == 30) {
    request[2] = request[3] = 2;
    request[0x1c / 4] = request[0x54 / 4] = 30;
    request[0x34 / 4] = request[0x48 / 4] = 10000000u / 8 / 30;
    request[0x50 / 4] = 10000000u / 30;
    request[0x58 / 4] = 0xff; /* accepts current source mode, no mode setter */
  }
  return 0;
}

static inline uint32_t pbs_ps4_pts90k_at_rate(uint64_t frame, unsigned fps) {
  return (uint32_t)(frame * (fps == 30 ? UINT64_C(3000) : UINT64_C(1500)));
}

/* FW11's Avcap copies request+0x30 into Venc preset+0x48. Native standard
 * QueryMemorySize accepts 3 and reports 17,858,560 direct bytes after 16K
 * alignment, versus 37,912,576 for the inherited RemotePlay value 9.
 * Including the pool's 16K prefix, 3 fits the real 24MiB standard allocation.
 * Keep this opt-in until real encoder/frame validation. No Neo flag needed. */
static inline int pbs_ps4_make_video_request_low_memory(
    uint32_t request[PBS_PS4_VIDEO_REQUEST_WORDS], unsigned height,
    unsigned usage, unsigned native_image_paste, unsigned fps) {
  if (height != 1080 || fps != 30 || usage || native_image_paste) return -1;
  if (pbs_ps4_make_video_request_at_rate(request, height, usage,
                                          native_image_paste, fps)) return -1;
  request[0x30 / 4] = PBS_PS4_MEMORY_COUNT;
  return 0;
}

/* Isolated FW11 RemotePlay 1080p30 experiment: primary24 selects preset
 * 0x10/variant2; secondary2 selects the generic compatibility check/source
 * mode. Native caps accepts this pair when PBS owns RemotePlay request3,
 * with the original daemon text and Neo bit clear. Still hardware-unverified. */
static inline int pbs_ps4_make_video_request_remote_1080(
    uint32_t request[PBS_PS4_VIDEO_REQUEST_WORDS], unsigned height,
    unsigned usage, unsigned native_image_paste, unsigned fps) {
  if (pbs_ps4_make_video_request_low_memory(request, height, usage,
                                             native_image_paste, fps)) return -1;
  request[0x08 / 4] = 24;
#if PBS_PS4_NATIVE_SOURCE
  request[0x0c / 4] = 24;
#endif
  return 0;
}

static inline int pbs_ps4_make_video_request(
    uint32_t request[PBS_PS4_VIDEO_REQUEST_WORDS], unsigned height,
    unsigned usage, unsigned native_image_paste) {
  return pbs_ps4_make_video_request_at_bitrate(request, height, usage,
      native_image_paste, PBS_PS4_DEFAULT_BITRATE_KBPS);
}
