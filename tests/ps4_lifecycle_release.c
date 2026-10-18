// SPDX-License-Identifier: GPL-3.0-or-later
/* Integrate the real capture wrapper and remote lifecycle. Reproduces the
 * hardware bug: PBS's own status=1 was mistaken for an external session. */
#define main legacy_remote_main
#include "ps4_remote_capture.c"
#undef main
#include "ps4_capture.h"
#include <dlfcn.h>

static int control_pending, enables, disables;
static int local_status(unsigned char *out) { *out = native_status; return 0; }
void *dlopen(const char *name, int flags) {
  (void)flags;
  return strstr(name,"libSceSystemService") ? (void *)(uintptr_t)2 : NULL;
}
void *dlsym(void *handle, const char *name) {
  return handle == (void *)(uintptr_t)2 &&
      !strcmp(name,"sceShellCoreUtilGetRemotePlayStatus") ? (void *)local_status : NULL;
}
int dlclose(void *handle) { (void)handle; return 0; }
char *dlerror(void) { return "host fixture"; }
int pbs_ps4_control(FILE *log, int enable, int *active) {
  (void)log;
  if (enable) ++enables;
  else {
    /* The foreign-session guard must run AFTER the owned status is clear. */
    assert(native_status == 0 && lifecycle_budget == 0);
    ++disables;
  }
  control_pending = enable;
  *active = enable;
  return 0;
}
int pbs_ps4_control_remote_init_pending(void) { return control_pending; }
int pbs_ps4_control_remote_term_uncertain(void) { return 0; }
int pbs_ps4_control_ownership_uncertain(void) { return 0; }
int pbs_ps4_control_gates_pending(void) { return 0; }
int pbs_ps4_control_restore_gates(FILE *log) { (void)log; return 0; }

int main(int argc, char **argv) {
  FILE *log = tmpfile();
  assert(log);
  native_module_enabled = 0;
  native_status = argc > 1 && !strcmp(argv[1],"foreign");
  assert(!pbs_ps4_capture_begin(log));
  if (native_status) {
    assert(pbs_ps4_init() < 0);
    assert(enables == 0 && disables == 0 && lifecycle_sets == 0);
  } else {
    assert(!pbs_ps4_init());
    assert(native_status == 1 && lifecycle_budget == 1 && enables == 1);
    if (argc > 1 && !strcmp(argv[1],"release-failed")) {
      lifecycle_fail_clear = 1;
      assert(pbs_ps4_term() < 0);
      assert(disables == 0 && control_pending && native_status == 1);
      pbs_ps4_capture_end();
      assert(lifecycle_sets == 2 && lifecycle_releases == 0 && disables == 0);
      assert(pbs_ps4_capture_begin(log) < 0);
      fclose(log);
      puts("PS4 failed lifecycle release quarantines global teardown without retries");
      return 0;
    }
    assert(!pbs_ps4_term());
    assert(native_status == 0 && lifecycle_budget == 0 && disables == 1);
    assert(!control_pending && lifecycle_sets == 2 && lifecycle_releases == 1);
    pbs_ps4_capture_end();
    assert(disables == 1 && lifecycle_sets == 2 && lifecycle_releases == 1);
  }
  fclose(log);
  puts("PS4 integrated lifecycle: owned status released before foreign-session guard; real native sessions refused");
}
