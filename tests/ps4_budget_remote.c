// SPDX-License-Identifier: GPL-3.0-or-later
/* Budget-only fixture: the daemon core-budget bit is armed and released
 * around the OpenVideoEx window without any code patch. */
#ifndef PBS_PS4_BUDGET_UNLOCK
#define PBS_PS4_BUDGET_UNLOCK 1
#endif
#define main unused_remote_fixture_main
#include "ps4_remote_capture.c"
#undef main
static int budget_fail, budget_arm, budget_sets;
int pbs_ps4_latch_budget_set(FILE *log, unsigned enable) {
  assert(log);
  if (budget_arm == (int)(enable ? 1 : 0)) return 0;
  ++budget_sets;
  if (budget_fail) return -1;
  budget_arm = enable ? 1 : 0;
  return 0;
}
int main(void) {
  FILE *log = tmpfile(); assert(log);
  uint32_t request[PBS_PS4_VIDEO_REQUEST_WORDS];
  assert(!pbs_ps4_make_video_request_at_rate(request,1080,0,0,30));
  assert(!pbs_ps4_remote_capture_begin(log));
  for (int scenario = 0; scenario < 3; ++scenario) {
    reset_counters(); budget_fail=scenario==1; budget_arm=0; budget_sets=0;
    forced_open_error=scenario==2?0x80a7000a:0;
    assert(!pbs_ps4_remote_capture_init());
    void *handle=NULL;
    int rc=pbs_ps4_remote_open(&handle,request);
    assert(budget_sets==(scenario==2?2:1));
    if (scenario==0) {
      assert(!rc && handle && budget_arm);
      assert(!pbs_ps4_remote_start(handle));
      assert(!pbs_ps4_remote_stop(handle) && !pbs_ps4_remote_close(handle));
    } else if (scenario==1) {
      assert(rc<0 && !open_calls && !handle && !budget_arm);
    } else {
      assert((uint32_t)rc==forced_open_error && !handle && open_calls==1 && !budget_arm);
    }
    assert(!pbs_ps4_remote_capture_end() && !budget_arm);
    assert(budget_sets==(scenario==1?1:2));
  }
  fclose(log);
  puts("Budget-only: armed open window, arm refusal before Open and failed-open release verified");
}
