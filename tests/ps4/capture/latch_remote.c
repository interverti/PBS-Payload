// SPDX-License-Identifier: GPL-3.0-or-later
/* Reuse the native ABI fixture, exercising only 1080p30 patch lifetimes. */
#ifndef PBS_PS4_BUDGET_UNLOCK
#define PBS_PS4_BUDGET_UNLOCK 0
#endif
#ifndef PBS_PS4_LOW_MEMORY
#define PBS_PS4_LOW_MEMORY 0
#endif
#define main unused_remote_fixture_main
#include "remote_capture.c"
#undef main
static int patch_pending, apply_fail, restore_fail, applies, restores;
int pbs_ps4_latch_patch_apply(FILE *log, const pbs_ps4_firmware *fw) {
  assert(log && fw && !patch_pending); ++applies;
  if (apply_fail) return -1;
  patch_pending = 1; return 0;
}
int pbs_ps4_latch_patch_restore(FILE *log) {
  assert(log); ++restores;
  if (restore_fail) return -1;
  patch_pending = 0; return 0;
}
int pbs_ps4_latch_patch_pending(void) { return patch_pending; }
#if PBS_PS4_BUDGET_UNLOCK
static int budget_fail, budget_arm, budget_sets;
int pbs_ps4_latch_budget_set(FILE *log, unsigned enable) {
  assert(log);
  if (budget_arm == (int)(enable ? 1 : 0)) return 0;
  ++budget_sets;
  if (budget_fail) return -1;
  budget_arm = enable ? 1 : 0;
  return 0;
}
#endif
int main(void) {
  FILE *log = tmpfile(); assert(log);
  uint32_t request[PBS_PS4_VIDEO_REQUEST_WORDS];
#if PBS_PS4_NATIVE_SOURCE
  assert(!pbs_ps4_make_video_request_remote_1080(request,1080,0,0,30));
#else
  assert(!pbs_ps4_make_video_request_at_rate(request,1080,0,0,30));
#endif
  assert(!pbs_ps4_remote_capture_begin(log));
  for (int scenario = 0; scenario < (PBS_PS4_BUDGET_UNLOCK ? 5 : 4); ++scenario) {
    reset_counters(); applies=restores=0; apply_fail=scenario==1; restore_fail=scenario==2;
#if PBS_PS4_BUDGET_UNLOCK
    budget_fail=scenario==4; budget_arm=0; budget_sets=0;
#endif
    forced_open_error=scenario==3?0x80a7000a:0;
    assert(!pbs_ps4_remote_capture_init());
    void *handle=NULL;
    int rc=pbs_ps4_remote_open(&handle,request);
    assert(applies==1);
    if (!PBS_PS4_LOW_MEMORY || scenario==1 || scenario==3) assert(restores>=1);
    else assert(!restores);
#if PBS_PS4_BUDGET_UNLOCK
    assert(budget_sets==(scenario==3?2:scenario==1?0:1));
    if (scenario==0||scenario==2) assert(budget_arm);
    if (scenario==3) assert(!budget_arm);
#endif
    if (scenario==1 || (PBS_PS4_BUDGET_UNLOCK && scenario==4)) assert(rc<0 && !open_calls && !handle);
    else if (scenario==3) assert((uint32_t)rc==forced_open_error && !handle && !patch_pending);
    else {
      assert(!rc && handle);
      if (PBS_PS4_LOW_MEMORY) {
        assert(patch_pending && !pbs_ps4_remote_start(handle) && start_calls==1);
        assert(!pbs_ps4_remote_stop(handle) && !pbs_ps4_remote_close(handle));
        assert(patch_pending); /* Restore only after the producer is closed. */
        if (scenario==2) {
          assert(pbs_ps4_remote_capture_end()<0 && patch_pending);
          restore_fail=0; /* Cleanup retains ownership and can retry. */
        }
      } else {
        if (scenario==2) {
          assert(patch_pending && pbs_ps4_remote_start(handle)<0 && !start_calls);
          restore_fail=0;
        } else assert(!patch_pending && !pbs_ps4_remote_start(handle));
        assert(!pbs_ps4_remote_stop(handle) && !pbs_ps4_remote_close(handle));
      }
    }
    assert(!pbs_ps4_remote_capture_end() && !patch_pending);
#if PBS_PS4_BUDGET_UNLOCK
    if (scenario==0||scenario==2||scenario==3) assert(budget_sets==2 && !budget_arm);
    else if (scenario==4) assert(budget_sets==1 && !budget_arm);
    else assert(!budget_sets && !budget_arm);
#endif
  }
  fclose(log);
  puts(PBS_PS4_LOW_MEMORY ? "Bounded low-memory latch: failed Open restore, Start/Stop/Close lifetime and cleanup retry verified" : "Open-only patch: refusal, successful Open, native error, restore before Start and final cleanup verified");
}
