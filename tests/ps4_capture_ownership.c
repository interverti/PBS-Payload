// SPDX-License-Identifier: GPL-3.0-or-later
/* Test the *real* PS4 Avcap dynamic wrapper against simulated native
 * RemotePlay ownership and failed static-request release. No Sony libs. */
#include "ps4_capture.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int rp_active, getter_absent, mode_getter_absent, failed_release, failed_remote_term, failed_local_term, failed_local_init;
static int init_calls, term_calls, enable_calls, disable_calls;
static int remote_request, remote_init_pending, remote_term_uncertain;
static int loaded_avsetting_sprx;
static int loaded_videoout_sprx;

static int fake_init(void) {
  init_calls++;
  if (failed_local_init) { failed_local_init = 0; return (int32_t)0x80a70001; }
  return 0;
}
static int fake_term(void) {
  term_calls++;
  if (failed_local_term) { failed_local_term = 0; return (int32_t)0x80a70001; }
  return 0;
}
static int fake_open(void **out, const void *params) { (void)out; (void)params; return 0; }
static int fake_control_handle(void *h) { (void)h; return 0; }
static int fake_info(void *h, unsigned t, void *out, size_t n) {
  (void)h; (void)t; (void)out; (void)n; return -1;
}
static int fake_read(void *h, void *buf, size_t size, void *info, int flags) {
  (void)h; (void)buf; (void)size; (void)info; (void)flags; return 0;
}
static int fake_set(void *h, unsigned type, const void *arg, size_t size) {
  (void)h; (void)type; (void)arg; (void)size; return 0;
}
static int fake_static(unsigned type, void *out, size_t size) {
  (void)type; if (size != 4) return -1;
  *(uint32_t *)out = 3;
  return 0;
}
static int fake_remoteplay(unsigned char *out) { *out = (unsigned char)rp_active; return 0; }
static int fake_output_mode(unsigned mode, unsigned arg1, unsigned arg2,
                            void *out, unsigned arg4, unsigned arg5,
                            unsigned long long size1, unsigned long long size2) {
  assert(mode == 0x7000 && !arg1 && !arg2 && !arg4 && !arg5);
  assert(size1 == 0x28 && size2 == 0x28);
  uint64_t value = 4;
  memcpy((unsigned char *)out + 0x10, &value, 8);
  return 0;
}
static int fake_videoout_mode(unsigned bus, unsigned arg1, unsigned arg2,
                              void *out, unsigned arg4, unsigned arg5,
                              unsigned long long size) {
  assert(bus == 0xff && !arg1 && !arg2 && !arg4 && !arg5 && size == 0x28);
  const uint32_t flags = 2;
  ((unsigned char *)out)[6] = 0x0d;
  memcpy((unsigned char *)out + 0x20, &flags, sizeof flags);
  return 0;
}

void *dlopen(const char *filename, int flags) {
  (void)flags;
  if (strstr(filename, "libSceAvcap")) return (void *)(uintptr_t)1;
  if (strstr(filename, "libSceShellCoreUtil")) return (void *)(uintptr_t)2;
  if (strstr(filename, "libSceAvSetting")) {
    if (strstr(filename, ".sprx")) loaded_avsetting_sprx++;
    return (void *)(uintptr_t)3;
  }
  if (strstr(filename, "libSceVideoOut")) {
    if (strstr(filename, ".sprx")) loaded_videoout_sprx++;
    return (void *)(uintptr_t)4;
  }
  return NULL;
}
char *dlerror(void) { return "mock dlopen missing"; }
int dlclose(void *handle) { assert((uintptr_t)handle <= 4); return 0; }
void *dlsym(void *handle, const char *name) {
  if ((uintptr_t)handle == 4) {
    if (!strcmp(name, "sceVideoOutGetVideoOutModeByBusSpecifier_")) return (void *)fake_videoout_mode;
    return NULL;
  }
  if ((uintptr_t)handle == 3) {
    if (!strcmp(name, "sceAvSettingGetCurrentOutputMode2_") && !mode_getter_absent)
      return (void *)fake_output_mode;
    return NULL;
  }
  if ((uintptr_t)handle == 2) {
    if (!strcmp(name, "sceShellCoreUtilGetRemotePlayStatus") && !getter_absent)
      return (void *)fake_remoteplay;
    return NULL;
  }
  if (!strcmp(name, "sceVshAvcapInit")) return (void *)fake_init;
  if (!strcmp(name, "sceVshAvcapTerm")) return (void *)fake_term;
  if (!strcmp(name, "sceVshAvcapOpenVideoEx")) return (void *)fake_open;
  if (!strcmp(name, "sceVshAvcapStart") || !strcmp(name, "sceVshAvcapStop") ||
      !strcmp(name, "sceVshAvcapClose")) return (void *)fake_control_handle;
  if (!strcmp(name, "sceVshAvcapGetInfo")) return (void *)fake_info;
  if (!strcmp(name, "sceVshAvcapGetStaticInfo")) return (void *)fake_static;
  if (!strcmp(name, "sceVshAvcapReadVideo")) return (void *)fake_read;
  if (!strcmp(name, "sceVshAvcapSetInfo")) return (void *)fake_set;
  return NULL;
}

int pbs_ps4_control(FILE *log, int enable, int *request_active) {
  (void)log;
  if (enable) {
    enable_calls++;
    remote_init_pending = 1;
    remote_request = *request_active = 1;
  } else {
    disable_calls++;
    if (failed_release) { failed_release = 0; return -1; }
    remote_request = *request_active = 0;
    remote_init_pending = 0;
    if (failed_remote_term) {
      failed_remote_term = 0;
      remote_term_uncertain = 1;
      return -1;
    }
  }
  return 0;
}
int pbs_ps4_control_remote_init_pending(void) { return remote_init_pending; }
int pbs_ps4_control_remote_term_uncertain(void) { return remote_term_uncertain; }
int pbs_ps4_control_ownership_uncertain(void) { return remote_term_uncertain; }
int pbs_ps4_control_gates_pending(void) { return 0; }
int pbs_ps4_control_restore_gates(FILE *log) { (void)log; return 0; }

static void reset_counts(void) {
  init_calls = term_calls = enable_calls = disable_calls = remote_request = 0;
  remote_init_pending = 0;
}

int main(int argc, char **argv) {
  FILE *log = tmpfile();
  assert(log);
  if (argc == 2 && !strcmp(argv[1], "native-activated")) {
    assert(pbs_ps4_capture_begin(log) == 0);
    rp_active = 0;
    assert(pbs_ps4_init() == 0);
    assert(init_calls == 1 && enable_calls == 1 && remote_request == 1);
    /* Foreign native RP starts after our initial inactive-state query.
     * A global value=0 would erase an unowned session; refuse all release. */
    rp_active = 1;
    assert(pbs_ps4_term() < 0);
    assert(disable_calls == 0 && term_calls == 0 && remote_request == 1);
    pbs_ps4_capture_end();
    assert(disable_calls == 0 && term_calls == 0);
    rp_active = 0; /* disappearing again must not undo the quarantine */
    assert(pbs_ps4_capture_begin(log) < 0);
    fclose(log);
    puts("PS4 midstream native RP activation quarantined without clearing global request");
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "local-init-negative")) {
    /* libSceAvcap Init FW11 rolls back its provisional refcount when the
     * operation returns a synchronous error. No local Term is owed, and
     * RemotePlay must not receive a capture request. */
    assert(pbs_ps4_capture_begin(log) == 0);
    failed_local_init = 1;
    assert(pbs_ps4_init() < 0);
    assert(init_calls == 1 && term_calls == 0 && enable_calls == 0);
    pbs_ps4_capture_end();
    assert(term_calls == 0 && disable_calls == 0);
    assert(pbs_ps4_capture_begin(log) == 0);
    assert(pbs_ps4_init() == 0);
    assert(init_calls == 2 && enable_calls == 1);
    assert(pbs_ps4_term() == 0);
    pbs_ps4_capture_end();
    assert(term_calls == 1 && disable_calls == 1);
    fclose(log);
    puts("PS4 negative local Init rolls back without an extra Term or RP mutation");
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "local-term-fail")) {
    assert(pbs_ps4_capture_begin(log) == 0);
    assert(pbs_ps4_init() == 0);
    failed_local_term = 1;
    assert(pbs_ps4_term() < 0);
    assert(term_calls == 1 && remote_request == 0);
    pbs_ps4_capture_end();
    assert(term_calls == 1); /* Refcount already decremented; no Term retry. */
    assert(pbs_ps4_capture_begin(log) < 0);
    assert(pbs_ps4_init() < 0);
    fclose(log);
    puts("PS4 local Avcap Term failure quarantines reentry and refcount");
    return 0;
  }
  rp_active = 1;
  assert(pbs_ps4_capture_begin(log) == 0);
  unsigned long long mode_value = 0;
  unsigned char raw[0x28];
  assert(pbs_ps4_output_mode_snapshot(&mode_value, raw) == 0 && mode_value == 4);
  assert(loaded_avsetting_sprx == 1);
  assert(raw[0x10] == 4 && raw[0x11] == 0);
  unsigned bus_flags = 0;
  assert(pbs_ps4_videoout_bus_snapshot(&bus_flags, raw) == 0 && bus_flags == 2);
  assert(loaded_videoout_sprx == 1 && raw[0x20] == 2 && raw[6] == 0x0d);
  assert(pbs_ps4_init() < 0);
  assert(!init_calls && !enable_calls && !remote_request);
  pbs_ps4_capture_end();

  reset_counts();
  rp_active = 0;
  assert(pbs_ps4_capture_begin(log) == 0);
  assert(pbs_ps4_init() == 0);
  assert(init_calls == 1 && enable_calls == 1 && remote_request == 1);
  failed_release = 1;
  assert(pbs_ps4_term() < 0);
  assert(term_calls == 0 && remote_request == 1);
  assert(pbs_ps4_term() == 0);
  assert(disable_calls == 2 && term_calls == 1 && remote_request == 0);
  pbs_ps4_capture_end();

  reset_counts();
  getter_absent = 1;
  mode_getter_absent = 1;
  rp_active = 1;
  assert(pbs_ps4_capture_begin(log) == 0);
  mode_value = 42;
  assert(pbs_ps4_output_mode_snapshot(&mode_value, raw) < 0 && mode_value == 0);
  assert(pbs_ps4_init() == 0); /* No unsupported hard dependency on getter. */
  assert(enable_calls == 1 && init_calls == 1);
  assert(pbs_ps4_term() == 0);
  pbs_ps4_capture_end();

  reset_counts();
  getter_absent = 0;
  rp_active = 0;
  assert(pbs_ps4_capture_begin(log) == 0);
  assert(pbs_ps4_init() == 0);
  failed_remote_term = 1;
  assert(pbs_ps4_term() < 0);
  assert(remote_request == 0 && term_calls == 1 && remote_term_uncertain == 1);
  assert(disable_calls == 1);
  pbs_ps4_capture_end();
  assert(disable_calls == 1); /* No second Term/SetStaticInfo after error. */
  assert(pbs_ps4_capture_begin(log) < 0);
  assert(pbs_ps4_init() < 0); /* No stale initialized/configured reuse. */
  fclose(log);
  puts("PS4 native RP ownership guard and checked remote release verified");
  return 0;
}
