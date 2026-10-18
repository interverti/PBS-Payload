// SPDX-License-Identifier: GPL-3.0-or-later
/* Regression oracle from FW11 SceAvCapture +0xb1c0..+0xb277. Use actual
 * MINSS/MAXSS NaN semantics: fminf/fmaxf have different NaN behavior. */
#include "ps4_video_request.h"
#include <assert.h>
#include <stdio.h>

static float minss(float a, float b) {
  /* Intrinsics may be constant-folded with different NaN selection by GCC.
   * Execute the firmware instruction, including operand order, literally. */
  __asm__ volatile("minss %1, %0" : "+x"(a) : "x"(b));
  return a;
}
static float maxss(float a, float b) {
  __asm__ volatile("maxss %1, %0" : "+x"(a) : "x"(b));
  return a;
}

/* For equal endpoints (zero rectangles or identical NaN bit patterns),
 * the service's sorting leaves both endpoints unchanged. These are exactly
 * the two request initializations being compared. */
static int full_screen_mask(const unsigned char rect[16]) {
  float p[4];
  memcpy(p, rect, sizeof p);
  /* Ordered equality at +0xb1c0/+0xb1cc skips degenerate rectangles;
   * unordered/NaN comparisons continue into the clamping code. */
  if (p[0] == p[2] || p[1] == p[3]) return 0;
  float left = minss(maxss(p[0], 0.0f), 1.0f);
  float top = minss(maxss(p[1], 0.0f), 1.0f);
  float right = minss(p[2], 1.0f);
  float bottom = minss(p[3], 1.0f);
  return left == 0.0f && top == 0.0f && right == 1.0f && bottom == 1.0f;
}

int main(void) {
  unsigned char old_rectangle[16];
  memset(old_rectangle, 0xff, sizeof old_rectangle);
  assert(full_screen_mask(old_rectangle));
  uint32_t request[PBS_PS4_VIDEO_REQUEST_WORDS];
  for (unsigned height = 720; height <= 1080; height += 360) {
    for (unsigned usage = 0; usage <= 1; ++usage) {
      for (unsigned paste = 0; paste <= 1; ++paste) {
        assert(!pbs_ps4_make_video_request(request, height, usage, paste));
        for (unsigned r = 0; r < 8; ++r) {
          const unsigned char *rect = (const unsigned char *)request + 0x88 + r * 16;
          assert(!full_screen_mask(rect));
          float p[4];
          memcpy(p, rect, sizeof p);
          assert(p[0] == p[2] && p[1] == p[3]);
        }
      }
    }
  }
  puts("FW11 privacy: old NaN rectangles mask full frame; PBS rectangles have zero area");
  return 0;
}
