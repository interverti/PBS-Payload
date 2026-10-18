// SPDX-License-Identifier: GPL-3.0-or-later
/* Verify the opt-in FW11 lifecycle refuses unavailable exports/active native
 * sessions and rolls back only its acquired budget after a pre-notify error. */
#define main legacy_remote_main
#include "remote_capture.c"
#undef main

int main(void) {
  FILE *log = tmpfile();
  assert(log);
  native_module_enabled = 0;
  assert(!pbs_ps4_remote_capture_begin(log));
  native_status = 1;
  assert(pbs_ps4_remote_capture_init() < 0);
  assert(lifecycle_acquires == 0 && lifecycle_sets == 0);
  native_status = 0;
  lifecycle_missing = 1;
  assert(pbs_ps4_remote_capture_init() < 0);
  assert(lifecycle_acquires == 0 && lifecycle_sets == 0);
  lifecycle_missing = 0;
  lifecycle_fail_pre = 1;
  assert(pbs_ps4_remote_capture_init() < 0);
  assert(lifecycle_acquires == 1 && lifecycle_releases == 1);
  assert(lifecycle_sets == 0 && native_status == 0 && !lifecycle_budget);
  lifecycle_fail_pre = 0;
  assert(!pbs_ps4_remote_capture_init());
  assert(lifecycle_budget && native_status == 1);
  assert(!pbs_ps4_remote_capture_end());
  assert(!lifecycle_budget && native_status == 0);
  assert(lifecycle_acquires == 2 && lifecycle_releases == 2 && lifecycle_sets == 2);
  assert(open_calls == 0 && start_calls == 0);
  lifecycle_fail_pre = 2;
  assert(pbs_ps4_remote_capture_init() < 0);
  assert(lifecycle_acquires == 3 && lifecycle_releases == 2);
  assert(lifecycle_budget == 1 && native_status == 0);
  /* Unknown result is quarantined, never converted to successful cleanup. */
  assert(pbs_ps4_remote_capture_init() < 0);
  assert(lifecycle_acquires == 3 && lifecycle_releases == 2);
  fclose(log);
  puts("FW11 lifecycle: provider resolution, active-session/missing-export refusal, rollback and checked state 0->1->0");
}
