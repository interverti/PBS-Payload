// SPDX-License-Identifier: GPL-3.0-or-later
/* PS4 uses its own Avcap IPC client; no PS5 capture ABI is reused. */
#include "platform/ps4/capture/capture.h"
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include "platform/ps4/system/control.h"
#if PBS_PS4_REMOTE_AVCAP
#include "platform/ps4/capture/remote.h"
#endif

#if !PBS_PS4_REMOTE_AVCAP
static void *library;
#endif
static void *shell_utility;
static void *video_settings;
static void *video_output;
static int shell_lookup_attempted;
static int video_lookup_attempted;
static int video_out_lookup_attempted;
static int (*get_rp_status)(unsigned char *);
static int (*get_current_output_mode)(unsigned, unsigned, unsigned, void *,
                                      unsigned, unsigned, unsigned long long,
                                      unsigned long long);
static int (*get_videoout_bus_mode)(unsigned, unsigned, unsigned, void *,
                                    unsigned, unsigned, unsigned long long);
static int initialized, configured;
static int local_term_uncertain;
static int teardown_quarantined;
static FILE *report;
#if !PBS_PS4_REMOTE_AVCAP
static int (*init)(void), (*term)(void);
static int (*open_video)(void **, const void *);
static int (*start)(void *), (*stop)(void *), (*close_video)(void *);
static int (*get_info)(void *, unsigned, void *, size_t);
static int (*get_static_info)(unsigned, void *, size_t);
static int (*read_video)(void *, void *, size_t, void *, int);
static int (*set_info)(void *, unsigned, const void *, size_t);
#endif

int pbs_ps4_capture_begin(FILE *log) {
  report = log;
  if (teardown_quarantined || local_term_uncertain ||
      pbs_ps4_control_ownership_uncertain()) {
    fprintf(log, "PS4 capture begin refused: unresolved previous capture ownership/teardown\n");
    return -1;
  }
#if PBS_PS4_REMOTE_AVCAP
  /* The remote backend attaches in pbs_ps4_init(), after the RemotePlay
   * control phase has performed Init/SetStaticInfo in that same process. */
  return pbs_ps4_remote_capture_begin(log);
#else
  library = dlopen("/system/common/lib/libSceAvcap.sprx", RTLD_LAZY);
  if (!library) { fprintf(log, "Avcap dlopen: %s\n", dlerror()); return -1; }
  init = dlsym(library, "sceVshAvcapInit");
  term = dlsym(library, "sceVshAvcapTerm");
  open_video = dlsym(library, "sceVshAvcapOpenVideoEx");
  start = dlsym(library, "sceVshAvcapStart");
  stop = dlsym(library, "sceVshAvcapStop");
  close_video = dlsym(library, "sceVshAvcapClose");
  get_info = dlsym(library, "sceVshAvcapGetInfo");
  get_static_info = dlsym(library, "sceVshAvcapGetStaticInfo");
  read_video = dlsym(library, "sceVshAvcapReadVideo");
  set_info = dlsym(library, "sceVshAvcapSetInfo");
  if (!init || !term || !open_video || !start || !stop || !close_video ||
      !get_info || !read_video || !set_info) {
    fprintf(log, "Missing PS4 Avcap exports: Init=%d Term=%d Open=%d Start=%d Stop=%d Close=%d GetInfo=%d Read=%d SetInfo=%d\n",
      !!init, !!term, !!open_video, !!start, !!stop, !!close_video, !!get_info, !!read_video, !!set_info);
    pbs_ps4_capture_end(); return -1;
  }
  return 0;
#endif
}

int pbs_ps4_term(void) {
  int failed = 0;
  if (teardown_quarantined) {
    fprintf(report, "Avcap release refused: capture teardown quarantined after ownership conflict\n");
#if PBS_PS4_REMOTE_AVCAP
    if (pbs_ps4_remote_capture_end() < 0) failed = -1;
#endif
    return -1;
  }
  if (pbs_ps4_control_ownership_uncertain()) {
    fprintf(report, "Avcap release quarantined: remote ownership uncertain; no global SetStaticInfo or Term\n");
#if PBS_PS4_REMOTE_AVCAP
    if (pbs_ps4_remote_capture_end() < 0) failed = -1;
#endif
    if (pbs_ps4_control_gates_pending() && pbs_ps4_control_restore_gates(report))
      fprintf(report, "WARNING: gate-only restore also failed in quarantine\n");
    return -1;
  }
#if PBS_PS4_REMOTE_AVCAP
  /* Our opt-in lifecycle sets this same global status bit. Release our
   * checked state before the foreign-session guard, or PBS mistakes itself
   * for a foreign session and leaves StaticInfo(21)/Init owned. */
  if (pbs_ps4_remote_prepare_release() < 0) {
    teardown_quarantined = 1;
    fprintf(report, "Avcap release quarantined: PBS lifecycle release failed\n");
    return -1;
  }
  /* Query while the retained remote context is still attached. */
  unsigned char remote_status = 0xff;
  int remote_status_rc = pbs_ps4_remoteplay_status(&remote_status);
#endif
  if (configured || pbs_ps4_control_remote_init_pending()) {
    /* We checked native RP occupancy before taking StaticInfo(21), but
     * another native session may have started during our stream. The global
     * value=0 setter is not scoped by a PBS-owned token: clearing it now
     * could disable that session. This is a conservative read-only guard,
     * not an atomic ownership guarantee or a scan-in readiness check. */
#if PBS_PS4_REMOTE_AVCAP
    unsigned char native_active = remote_status;
    int status_rc = remote_status_rc;
#else
    unsigned char native_active = 0xff;
    int status_rc = pbs_ps4_remoteplay_status(&native_active);
#endif
    if (status_rc >= 0 && native_active) {
      fprintf(report, "WARNING: native RemotePlay became active before release (status=%u); keeping global capture request and quarantining teardown\n",
              native_active);
      teardown_quarantined = 1;
#if PBS_PS4_REMOTE_AVCAP
      if (pbs_ps4_remote_capture_end() < 0) failed = -1;
#endif
      if (pbs_ps4_control_gates_pending() && pbs_ps4_control_restore_gates(report))
        fprintf(report, "WARNING: gate-only restore also failed during native RemotePlay ownership conflict\n");
      return -1;
    }
#if PBS_PS4_REMOTE_AVCAP
    if (pbs_ps4_remote_capture_end() < 0) failed = -1;
#endif
    int rc = pbs_ps4_control(report, 0, &configured);
    if (rc < 0) failed = rc;
  }
#if PBS_PS4_REMOTE_AVCAP
  /* Even without a configured request, release the captured target memory
   * and detach from SceRemotePlay before the control/Term phase. */
  if (pbs_ps4_remote_capture_active() && pbs_ps4_remote_capture_end() < 0)
    failed = -1;
#endif
  if (pbs_ps4_control_gates_pending() && pbs_ps4_control_restore_gates(report)) failed = -1;
  /* A failed remote release may still own the global capture request.
   * Keep the local Avcap IPC client initialized until the next checked
   * cleanup attempt; never hide that failure behind a successful Term. */
  if (configured || pbs_ps4_control_remote_init_pending() || pbs_ps4_control_gates_pending()) {
    fprintf(report, "Avcap local Term deferred: pending RemotePlay Init/request or gate restore\n");
    return failed ? failed : -1;
  }
#if !PBS_PS4_REMOTE_AVCAP
  if (initialized) {
    int rc = term();
    /* Native libSceAvcap Term decrements its refcount before teardown.
     * A negative result does not prove the decrement did not happen. */
    initialized = 0;
    if (rc < 0) {
      local_term_uncertain = 1;
      fprintf(report, "WARNING: local Avcap Term failed (%08x); will not retry refcount decrement\n", (unsigned)rc);
      failed = rc;
    }
  }
#endif
  if (local_term_uncertain) failed = -1;
  if (pbs_ps4_control_remote_term_uncertain()) {
#if PBS_PS4_REMOTE_AVCAP
    fprintf(report, "RemotePlay Term uncertain; remote Avcap reference requires inspection\n");
#else
    fprintf(report, "RemotePlay Term uncertain; local Avcap released but remote reference requires inspection\n");
#endif
    failed = -1;
  }
#if PBS_PS4_REMOTE_AVCAP
  initialized = 0;
#endif
  return failed;
}

void pbs_ps4_capture_end(void) {
  if (shell_utility) { dlclose(shell_utility); shell_utility = NULL; }
  if (video_settings) { dlclose(video_settings); video_settings = NULL; }
  if (video_output) { dlclose(video_output); video_output = NULL; }
  get_rp_status = NULL;
  get_current_output_mode = NULL;
  get_videoout_bus_mode = NULL;
  shell_lookup_attempted = 0;
  video_lookup_attempted = 0;
  video_out_lookup_attempted = 0;
#if PBS_PS4_REMOTE_AVCAP
  if (pbs_ps4_remote_capture_active() || configured ||
      pbs_ps4_control_remote_init_pending() ||
      pbs_ps4_control_gates_pending() ||
      pbs_ps4_control_ownership_uncertain()) {
    int cleanup_rc = pbs_ps4_term();
    if (cleanup_rc < 0 || configured || initialized ||
        pbs_ps4_control_remote_init_pending() ||
        pbs_ps4_control_gates_pending() ||
        pbs_ps4_control_ownership_uncertain()) {
      teardown_quarantined = 1;
      fprintf(report, "WARNING: remote Avcap cleanup incomplete (%08x); quarantining this process\n",
              (unsigned)cleanup_rc);
    }
  }
  /* Never leave the retained SceRemotePlay trace or the FW11 debug gates
   * open after teardown, even when a quarantine path declined the global
   * SetStaticInfo/Term release. */
  if (pbs_ps4_control_ctx()) pbs_ps4_control_ctx_release();
  if (pbs_ps4_control_gates_pending() && pbs_ps4_control_restore_gates(report))
    fprintf(report, "WARNING: final PS4 debug gate restore failed\n");
#else
  if (library) {
    int cleanup_rc = 0;
    if (initialized || configured || pbs_ps4_control_remote_init_pending() ||
        pbs_ps4_control_gates_pending() || pbs_ps4_control_ownership_uncertain())
      cleanup_rc = pbs_ps4_term();
    if (cleanup_rc < 0 || initialized || configured ||
        pbs_ps4_control_remote_init_pending() ||
        pbs_ps4_control_gates_pending() ||
        pbs_ps4_control_ownership_uncertain() || local_term_uncertain) {
      teardown_quarantined = 1;
      fprintf(report, "WARNING: Avcap cleanup incomplete (%08x); retaining local library and quarantining this process\n",
              (unsigned)cleanup_rc);
      return;
    }
    dlclose(library);
    library = NULL;
  }
#endif
}

int pbs_ps4_init(void) {
  if (teardown_quarantined) {
    fprintf(report, "PS4 capture init refused: previous teardown not complete\n");
    return -1;
  }
  if (local_term_uncertain) {
    fprintf(report, "PS4 capture refused: prior local Avcap Term outcome uncertain\n");
    return -1;
  }
  if (initialized) return configured ? 0 : -1;
  if (pbs_ps4_control_ownership_uncertain()) {
    fprintf(report, "PS4 capture refused: uncertain RemotePlay ownership from prior call\n");
    return -1;
  }
  /* Native RemotePlay can own the global capture pipeline. Its status is
   * observable independently of Avcap's internal resource bitmap. Never
   * overwrite a known active native session's StaticInfo(21) request. */
  unsigned char native_active = 0xff;
  int native_rc = pbs_ps4_remoteplay_status(&native_active);
  if (native_rc >= 0 && native_active) {
    fprintf(report, "PS4 native RemotePlay active (status=%u): refusing shared capture ownership\n",
            native_active);
    return -1;
  }
  if (pbs_ps4_control_remote_term_uncertain()) {
    fprintf(report, "Refusing new capture: previous RemotePlay Term outcome uncertain\n");
    return -1;
  }
  if (native_rc < 0) {
    fprintf(report, "Native RemotePlay occupancy unavailable; continuing with Avcap ownership checks\n");
  }
#if PBS_PS4_REMOTE_AVCAP
  /* Native RemotePlay setup +2fa545 requests 60 fps capture and cleanup
   * +2fa92b releases it before Term. The control phase performs that setup
   * inside SceRemotePlay; the capture backend then attaches to the same
   * process and uses the already-initialized native client class. */
  int rc = pbs_ps4_control(report, 1, &configured);
  if (rc < 0) return rc;
  rc = pbs_ps4_remote_capture_init();
  if (rc < 0) {
    int release = pbs_ps4_control(report, 0, &configured);
    if (release < 0)
      fprintf(report, "WARNING: remote control rollback after attach failure failed\n");
    return rc;
  }
  initialized = 1;
  return 0;
#else
  int rc = init();
  if (rc < 0) return rc;
  initialized = 1;
  /* Native RemotePlay setup +2fa545 requests 60 fps capture; native cleanup
   * +2fa92b releases the request with value zero before Term. */
  rc = pbs_ps4_control(report, 1, &configured);
  return rc;
#endif
}
int pbs_ps4_remoteplay_status(unsigned char *status) {
  if (!status) return -1;
  *status = 0xff;
#if PBS_PS4_REMOTE_AVCAP
  if (pbs_ps4_remote_capture_active())
    return pbs_ps4_remote_status(status);
#endif
  if (!shell_lookup_attempted) {
    shell_lookup_attempted = 1;
    /* FW11 imports library ShellCoreUtil from provider module
     * libSceSystemService (#W#N), not a separate ShellCoreUtil SPRX.
     * This is a read-only query. Never change RemotePlay global status. */
    shell_utility = dlopen("/system/common/lib/libSceSystemService.sprx", RTLD_LAZY);
    if (!shell_utility) shell_utility = dlopen("libSceSystemService.sprx", RTLD_LAZY);
    if (!shell_utility) shell_utility = dlopen("/system/common/lib/libSceShellCoreUtil.sprx", RTLD_LAZY);
    if (!shell_utility) shell_utility = dlopen("libSceShellCoreUtil.sprx", RTLD_LAZY);
    if (shell_utility)
      get_rp_status = dlsym(shell_utility, "sceShellCoreUtilGetRemotePlayStatus");
    if (!get_rp_status) fprintf(report, "ShellCore RemotePlay status getter unavailable (observation only)\n");
  }
  return get_rp_status ? get_rp_status(status) : -1;
}
int pbs_ps4_output_mode_snapshot(unsigned long long *descriptor,
                                 unsigned char raw_output[0x28]) {
  if (!descriptor) return -1;
  *descriptor = 0;
  if (!video_lookup_attempted) {
    video_lookup_attempted = 1;
    /* FW11 SceShellCore +0x2e63aa..+0x2e641b invokes the getter with
     * exactly 8 SysV arguments, zeroed 0x28-byte result, then examines the
     * qword at result+0x10. No SceAvSetting setter is called by PBS. */
    /* FTP verification on FW11 exposes /system/common/lib/libSceAvSetting.sprx.
     * ShellCore imports the logical .prx dependency name, which is not
     * necessarily a loadable path in a standalone homebrew process. */
    video_settings = dlopen("/system/common/lib/libSceAvSetting.sprx", RTLD_LAZY);
    if (!video_settings) video_settings = dlopen("libSceAvSetting.sprx", RTLD_LAZY);
    if (!video_settings) video_settings = dlopen("/system/common/lib/libSceAvSetting.prx", RTLD_LAZY);
    if (!video_settings) video_settings = dlopen("libSceAvSetting.prx", RTLD_LAZY);
    if (video_settings)
      get_current_output_mode = dlsym(video_settings, "sceAvSettingGetCurrentOutputMode2_");
    if (!get_current_output_mode)
      fprintf(report, "SceAvSetting output getter unavailable (observation only)\n");
  }
  if (!get_current_output_mode) return -1;
  unsigned char buffer[0x28] __attribute__((aligned(32))) = {0};
  int rc = get_current_output_mode(0x7000, 0, 0, buffer, 0, 0, 0x28, 0x28);
  if (rc == 0) {
    memcpy(descriptor, buffer + 0x10, sizeof *descriptor);
    if (raw_output) memcpy(raw_output, buffer, sizeof buffer);
  }
  return rc;
}
int pbs_ps4_videoout_bus_snapshot(unsigned *flags, unsigned char raw_output[0x28]) {
  if (!flags) return -1;
  *flags = 0;
  if (!video_out_lookup_attempted) {
    video_out_lookup_attempted = 1;
    /* Native RemotePlay FW11 +0x10510/+0x1064d: bus=0xff, no flags,
     * writable output[0x28], last parameter 0x28. Import NID UazrNFzZPRU.
     * Open the actual .sprx observed on the console; the import uses .prx. */
    video_output = dlopen("/system/common/lib/libSceVideoOut.sprx", RTLD_LAZY);
    if (!video_output) video_output = dlopen("libSceVideoOut.sprx", RTLD_LAZY);
    if (!video_output) video_output = dlopen("libSceVideoOut.prx", RTLD_LAZY);
    if (video_output) {
      get_videoout_bus_mode = dlsym(video_output, "sceVideoOutGetVideoOutModeByBusSpecifier_");
    }
    if (!get_videoout_bus_mode)
      fprintf(report, "VideoOut bus mode getter unavailable (observation only)\n");
  }
  if (!get_videoout_bus_mode) return -1;
  unsigned char data[0x28] __attribute__((aligned(16))) = {0};
  int rc = get_videoout_bus_mode(0xff, 0, 0, data, 0, 0, 0x28);
  if (rc >= 0) {
    memcpy(flags, data + 0x20, sizeof *flags);
    if (raw_output) memcpy(raw_output, data, sizeof data);
  }
  return rc;
}
#if PBS_PS4_REMOTE_AVCAP
int pbs_ps4_open(void **handle, const void *params) {
  return pbs_ps4_remote_capture_active() ? pbs_ps4_remote_open(handle, params) : -1;
}
int pbs_ps4_start(void *handle) {
  return pbs_ps4_remote_capture_active() ? pbs_ps4_remote_start(handle) : -1;
}
int pbs_ps4_stop(void *handle) {
  return pbs_ps4_remote_capture_active() ? pbs_ps4_remote_stop(handle) : -1;
}
int pbs_ps4_close(void *handle) {
  return pbs_ps4_remote_capture_active() ? pbs_ps4_remote_close(handle) : -1;
}
int pbs_ps4_info(void *handle, unsigned type, void *out, size_t size) {
  return pbs_ps4_remote_capture_active()
             ? pbs_ps4_remote_info(handle, type, out, size) : -1;
}
int pbs_ps4_static_info(unsigned type, void *out, size_t size) {
  return pbs_ps4_remote_capture_active()
             ? pbs_ps4_remote_static_info(type, out, size) : -1;
}
int pbs_ps4_read(void *handle, void *out, size_t size, void *info, int flags) {
  return pbs_ps4_remote_capture_active()
             ? pbs_ps4_remote_read(handle, out, size, info, flags) : -1;
}
int pbs_ps4_keyframe(void *handle) {
  /* RemotePlay +2fb073 requests an intra frame using info type 3. */
  return pbs_ps4_remote_capture_active()
             ? pbs_ps4_remote_keyframe(handle) : -1;
}
#else
int pbs_ps4_open(void **handle, const void *params) { return open_video(handle, params); }
int pbs_ps4_start(void *handle) { return start(handle); }
int pbs_ps4_stop(void *handle) { return stop(handle); }
int pbs_ps4_close(void *handle) { return close_video(handle); }
int pbs_ps4_info(void *handle, unsigned type, void *out, size_t size) {
  return get_info(handle, type, out, size);
}
int pbs_ps4_static_info(unsigned type, void *out, size_t size) {
  return get_static_info ? get_static_info(type, out, size) : -1;
}
int pbs_ps4_read(void *handle, void *out, size_t size, void *info, int flags) {
  return read_video(handle, out, size, info, flags);
}
int pbs_ps4_keyframe(void *handle) {
  /* RemotePlay +2fb073 requests an intra frame using info type 3. */
  return set_info(handle, 3, NULL, 0);
}
#endif
