// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdio.h>
#include "platform/ps5/capture_cleanup.h"

typedef struct {
  int calls, fail_at, references;
  pbs_ps5_release_step sequence[3];
} fixture;

static int release(void *opaque, pbs_ps5_release_step step) {
  fixture *f = opaque;
  assert(f->calls < 3);
  f->sequence[f->calls++] = step;
  if (f->calls == f->fail_at) return -1;
  if (step == PBS_PS5_RELEASE_INITIALIZE) --f->references;
  return 0;
}

int main(void) {
  assert(pbs_ps5_callback_already_registered((long)0x819b0007u));
  assert(!pbs_ps5_callback_already_registered(0));
  assert(!pbs_ps5_callback_already_registered((long)0x819b0006u));

  /* Reopening must not accumulate our Initialize references or consume the
   * references held by native RemotePlay and another client. */
  fixture f = {.references = 2};
  for (unsigned cycle = 0; cycle < 1000; ++cycle) {
    pbs_ps5_capture_ownership owned = {1, 1, 1, 0};
    ++f.references;
    f.calls = 0;
    assert(!pbs_ps5_release_capture(&owned, 1, release, &f));
    assert(f.calls == 3 && f.references == 2);
    assert(f.sequence[0] == PBS_PS5_RELEASE_STOP);
    assert(f.sequence[1] == PBS_PS5_RELEASE_CALLBACK);
    assert(f.sequence[2] == PBS_PS5_RELEASE_INITIALIZE);
    assert(!owned.initialized && !owned.callback && !owned.running);
    assert(!pbs_ps5_release_capture(&owned, 1, release, &f));
    assert(f.calls == 3 && f.references == 2);
  }

  /* Failure at any release stage must stop subsequent operations and retries. */
  for (int fail = 1; fail <= 3; ++fail) {
    pbs_ps5_capture_ownership owned = {1, 1, 1, 0};
    f = (fixture){.fail_at = fail, .references = 3};
    assert(pbs_ps5_release_capture(&owned, 1, release, &f));
    assert(f.calls == fail && f.references == 3 && owned.uncertain);
    assert(owned.running == (fail == 1));
    assert(owned.callback == (fail <= 2));
    assert(owned.initialized);
    assert(pbs_ps5_release_capture(&owned, 1, release, &f));
    assert(f.calls == fail);
  }

  /* Unclosed handles and interrupted acquisitions must retain ownership. */
  for (int uncertain = 0; uncertain <= 1; ++uncertain) {
    pbs_ps5_capture_ownership owned = {1, 1, 1, uncertain};
    f = (fixture){0};
    assert(pbs_ps5_release_capture(&owned, uncertain, release, &f));
    assert(!f.calls && owned.initialized && owned.callback && owned.running);
  }

  /* Each partial acquisition releases only the operations that succeeded. */
  for (unsigned mask = 0; mask < 8; ++mask) {
    pbs_ps5_capture_ownership owned = {!!(mask & 1), !!(mask & 2), !!(mask & 4), 0};
    f = (fixture){.references = 2 + !!(mask & 1)};
    assert(!pbs_ps5_release_capture(&owned, 1, release, &f));
    int next = 0;
    if (mask & 4) assert(f.sequence[next++] == PBS_PS5_RELEASE_STOP);
    if (mask & 2) assert(f.sequence[next++] == PBS_PS5_RELEASE_CALLBACK);
    if (mask & 1) assert(f.sequence[next++] == PBS_PS5_RELEASE_INITIALIZE);
    assert(f.calls == next && f.references == 2);
  }

  /* A pre-existing callback is borrowed: release our RUNNING transition and
   * Initialize reference, but never clear somebody else's callback. */
  pbs_ps5_capture_ownership borrowed = {1, 0, 1, 0};
  f = (fixture){.references = 3};
  assert(!pbs_ps5_release_capture(&borrowed, 1, release, &f));
  assert(f.calls == 2 && f.references == 2);
  assert(f.sequence[0] == PBS_PS5_RELEASE_STOP);
  assert(f.sequence[1] == PBS_PS5_RELEASE_INITIALIZE);
  puts("PS5 capture cleanup: ownership, release ordering and failure tests passed");
  return 0;
}
