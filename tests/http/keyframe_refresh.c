// SPDX-License-Identifier: GPL-3.0-or-later
/* Exercise the shared PS4/PS5 policy without a browser or wall-clock sleeps. */
#include "common/http/server.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static _Atomic uint64_t now_ms = 1000;
int __real_clock_gettime(clockid_t id, struct timespec *t);
int __wrap_clock_gettime(clockid_t id, struct timespec *t) {
  if (id != CLOCK_MONOTONIC) return __real_clock_gettime(id, t);
  uint64_t now = atomic_load(&now_ms);
  t->tv_sec = now / 1000;
  t->tv_nsec = (now % 1000) * 1000000;
  return 0;
}

int main(void) {
  const unsigned char sps[] = {0x67,0x4d,0x40,0x28}, pps[] = {0x68,0xee};
  const unsigned char idr[] = {0,0,0,1,0x65,0x88};
  const unsigned char predicted[] = {0,0,1,0x41,0x88};
  assert(!pbs_h264_server_start(18140,1280,720,sps,sizeof sps,pps,sizeof pps));
  assert(pbs_h264_take_keyframe_request());
  assert(!pbs_h264_take_keyframe_request());
  /* Tiny, repetitive static-scene P frames must not reset the IDR timer. */
  for (uint64_t t = 1016; t < 3000; t += 16) {
    atomic_store(&now_ms, t);
    assert(!pbs_h264_publish(predicted,sizeof predicted,(uint32_t)(t * 90)));
    assert(!pbs_h264_take_keyframe_request());
  }
  atomic_store(&now_ms, 3000);
  assert(pbs_h264_take_keyframe_request());
  /* Refused/missing IDR: retry after two seconds, never on every poll. */
  atomic_store(&now_ms, 4999);
  assert(!pbs_h264_take_keyframe_request());
  atomic_store(&now_ms, 5000);
  assert(pbs_h264_take_keyframe_request());
  atomic_store(&now_ms, 6500);
  assert(!pbs_h264_publish(idr,sizeof idr,585000));
  atomic_store(&now_ms, 7000);
  assert(!pbs_h264_take_keyframe_request());
  atomic_store(&now_ms, 8500);
  assert(pbs_h264_take_keyframe_request());
  /* Explicit recovery remains immediate, even inside the periodic interval. */
  atomic_store(&now_ms, 8501);
  pbs_h264_discontinuity();
  assert(pbs_h264_take_keyframe_request());
  assert(!pbs_h264_take_keyframe_request());
  /* Reopening an encoder must reset the old session's refresh deadlines. */
  atomic_store(&now_ms, 10000);
  assert(!pbs_h264_server_start(18140,1920,1080,sps,sizeof sps,pps,sizeof pps));
  assert(pbs_h264_take_keyframe_request());
  atomic_store(&now_ms, 11999);
  assert(!pbs_h264_take_keyframe_request());
  atomic_store(&now_ms, 12000);
  assert(pbs_h264_take_keyframe_request());
  pbs_h264_server_stop();
  atomic_store(&now_ms, 20000);
  assert(!pbs_h264_take_keyframe_request());
  pbs_h264_server_wait();
  puts("Shared encoder refresh: static frames, retry throttle, IDR receipt and reopen verified");
  return 0;
}
