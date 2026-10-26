// SPDX-License-Identifier: GPL-3.0-or-later
/* PS4 Avcap backend. The HTTP viewer and RTP transport are shared with PS5. */
#include "common/http/server.h"
#include "common/diagnostics/problem_log.h"
#include "platform/ps4/capture/capture.h"
#include "platform/ps4/capture/request.h"
#include "platform/ps4/system/control.h"
#include "platform/ps4/system/firmware.h"
#if PBS_PS4_VENC_JOURNAL
#include "platform/ps4/diagnostics/venc_journal.h"
#endif
#if PBS_PS4_LOW_MEMORY || defined(PBS_PS4_ENCODER_STATE)
#include "platform/ps4/diagnostics/admission.h"
#endif
#ifdef PBS_PS4_DAEMON_DUMP
#include "platform/ps4/diagnostics/daemon_dump.h"
#endif
#ifdef PBS_PS4_SMOKE_WAIT_SOURCE
#include "platform/ps4/capture/source_state.h"
#endif
#include "common/media/buffer.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ps4/kernel.h>
#ifndef PBS_PS4_DATA_ROOT
#define PBS_PS4_DATA_ROOT "/data/pbs"
#endif
#define PBS_PS4_CAPTURE_DIR PBS_PS4_DATA_ROOT "/ps4"
#ifndef PBS_PS4_CAPTURE_FPS
#define PBS_PS4_CAPTURE_FPS 60
#endif
#if PBS_PS4_CAPTURE_FPS != 30 && PBS_PS4_CAPTURE_FPS != 60
#error Unsupported capture rate
#endif
#ifndef PBS_PS4_CAPTURE_HEIGHT
#define PBS_PS4_CAPTURE_HEIGHT 720
#endif
#if PBS_PS4_CAPTURE_HEIGHT != 720 && PBS_PS4_CAPTURE_HEIGHT != 1080
#error Unsupported PS4 default capture height
#endif
#ifndef PBS_PS4_CAPTURE_USAGE
#define PBS_PS4_CAPTURE_USAGE 0
#endif
#ifndef PBS_PS4_IMAGE_PASTE
#define PBS_PS4_IMAGE_PASTE 0
#endif
/* FW11 ReadVideo(flags=0) can loop indefinitely on repeated positive event
 * wakeups with an empty producer ring. Native stop/close from another thread
 * is unsafe while this call is in progress. Keep the synchronous reader
 * cancellable at its call boundaries using the verified nonwaiting ABI.
 * The native wait mode remains opt-in for controlled comparative smoke tests. */
#ifndef PBS_PS4_READ_WAIT_EVENTS
#define PBS_PS4_READ_WAIT_EVENTS 0
#endif
#if PBS_PS4_READ_WAIT_EVENTS != 0 && PBS_PS4_READ_WAIT_EVENTS != 1
#error PBS_PS4_READ_WAIT_EVENTS must be 0 or 1
#endif
#if PBS_PS4_CAPTURE_USAGE != 0 && PBS_PS4_CAPTURE_USAGE != 1
#error Unsupported PS4 Avcap usage; inspect the service ABI before enabling other modes.
#endif
#ifdef PBS_PS4_CAPTURE_SECONDS
#include <signal.h>
#include <ucontext.h>
static int crash_fd = -1;
static void crash_report(int sig, siginfo_t *info, void *context) {
  char line[] = "CRASH sig=00 rip=0000000000000000 address=0000000000000000\n";
  const char digits[] = "0123456789abcdef";
  uint64_t rip = ((ucontext_t *)context)->uc_mcontext.mc_rip;
  uint64_t address = (uintptr_t)info->si_addr;
  line[10] = digits[(sig >> 4) & 15]; line[11] = digits[sig & 15];
  for (int i = 0; i < 16; ++i) {
    /* Offsets 16 and 41 are the literal '=' separators. Older reports
     * overwrote them and retained an extra trailing zero, obscuring RIP. */
    line[17 + i] = digits[(rip >> ((15 - i) * 4)) & 15];
    line[42 + i] = digits[(address >> ((15 - i) * 4)) & 15];
  }
  if (crash_fd >= 0) write(crash_fd, line, sizeof line - 1);
  /* The compact problem index must state the crash too: it is the first file
   * a failed run is inspected with. Raw write, safe inside a handler. */
  pbs_problem_raw(line, sizeof line - 1);
  _exit(128 + sig);
}
#endif

static uint64_t milliseconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void ps4_static_state(FILE *log, const char *stage) {
  uint32_t capture_request = UINT32_MAX;
  int rc = pbs_ps4_static_info(21, &capture_request, sizeof capture_request);
  if (rc < 0) {
    fprintf(log, "Avcap static-info(21) %s: unavailable rc=%08x\n", stage, (unsigned)rc);
  } else {
    /* Native RemotePlay explicitly tests bit 0x20; bits 1/2 are set
     * during the setup handshake, but their meaning is not established. */
    fprintf(log, "Avcap static-info(21) %s: rc=%08x flags=%08x request_low=%u native_bit5=%u\n",
            stage, (unsigned)rc, capture_request, capture_request & 3u,
            !!(capture_request & 0x20u));
  }
#ifdef PBS_PS4_OBSERVE_REMOTEPLAY_STATUS
  unsigned char remoteplay = 0xff;
  int rp_rc = pbs_ps4_remoteplay_status(&remoteplay);
  if (rp_rc < 0) fprintf(log, "ShellCore RemotePlay status %s: unavailable rc=%08x\n", stage, (unsigned)rp_rc);
  else fprintf(log, "ShellCore RemotePlay status %s: rc=%08x value=%u\n", stage, (unsigned)rp_rc, remoteplay);
#endif
#ifdef PBS_PS4_OBSERVE_VIDEO_MODE
  unsigned long long mode_descriptor = 0;
  unsigned char mode_raw[0x28] = {0};
  int mode_rc = pbs_ps4_output_mode_snapshot(&mode_descriptor, mode_raw);
  if (mode_rc < 0) fprintf(log, "SceAvSetting output snapshot %s: unavailable rc=%08x\n", stage, (unsigned)mode_rc);
  else if (mode_rc == 0) {
    fprintf(log, "SceAvSetting output snapshot %s: rc=%08x base_mode=%llu raw40=",
            stage, (unsigned)mode_rc, mode_descriptor);
    for (int n = 0; n < 0x28; ++n) fprintf(log, "%02x", mode_raw[n]);
    fputc('\n', log);
  } else fprintf(log, "SceAvSetting output snapshot %s: unexpected rc=%08x\n", stage, (unsigned)mode_rc);
#endif
#ifdef PBS_PS4_OBSERVE_VIDEOOUT_BUS
  unsigned bus_flags = 0;
  unsigned char bus_raw[0x28] = {0};
  int bus_rc = pbs_ps4_videoout_bus_snapshot(&bus_flags, bus_raw);
  if (bus_rc < 0) {
    fprintf(log, "SceVideoOut bus snapshot %s: unavailable rc=%08x\n", stage, (unsigned)bus_rc);
  } else {
    /* Native ShellCore reads byte+6 and considers values 0x0c..0x11;
     * RemotePlay uses bit1 at +0x20. Neither proves pixel ownership. */
    fprintf(log, "SceVideoOut bus snapshot %s: rc=%08x enum_byte6=%02x flags=%08x native_bit1=%u raw40=",
            stage, (unsigned)bus_rc, bus_raw[6], bus_flags, !!(bus_flags & 2));
    for (int n = 0; n < 0x28; ++n) fprintf(log, "%02x", bus_raw[n]);
    fputc('\n', log);
  }
#endif
}

#if defined(PBS_PS4_CAPTURE_SECONDS) || defined(PBS_PS4_TEST_OPEN_SLOTS)
/* FW11 SceAvCapture::GetStaticInfo(type=24) directly reads its 32-bit
 * encoder-slot allocation bitmap (service +0xcb0 -> +0xdb4 -> +0x6ae0).
 * For usage=0/profile=19 the service's admission check +0x7fef..0x800a
 * tests bit 0. This is a *read-only* cause discriminator after 0x80a70008:
 * nonzero bit0 is evidence of occupied encoder resources, while clear bit0
 * does not prove another admission predicate succeeded. No bit is changed,
 * no slot is released or reassigned and no extra RemotePlay owner is made. */
static void ps4_open_slot_snapshot(FILE *log, const char *stage) {
  uint32_t bitmap = 0;
  const int rc = pbs_ps4_static_info(24, &bitmap, sizeof bitmap);
  if (rc == (int)sizeof bitmap)
    fprintf(log, "Avcap encoder slots %s: mask=%08x slot0=%u (read-only)\n",
            stage, bitmap, bitmap & 1u);
  else
    fprintf(log, "Avcap encoder slots %s: unavailable rc=%08x\n", stage, (unsigned)rc);
}
#endif

#ifdef PBS_PS4_SMOKE_WAIT_SOURCE
/* A conservative, bounded observation window between Open and Start. FW11
 * ChangeOutputMode2 only ACKs its request while READY_SCANIN arrives later
 * asynchronously in the SceAvCapture service. PBS does not own/subscribe
 * that service callback and must never pretend these snapshots prove pixels.
 * Production does not use this experimental timing barrier. */
static pbs_ps4_source_state ps4_sample_output_state(void) {
  pbs_ps4_source_state state = {0};
  unsigned char raw[0x28];
  unsigned long long mode = 0;
  if (pbs_ps4_output_mode_snapshot(&mode, raw) == 0) {
    state.mode_valid = 1;
    state.base_mode = mode;
  }
  unsigned flags = 0;
  if (pbs_ps4_videoout_bus_snapshot(&flags, raw) == 0) {
    state.bus_valid = 1;
    state.bus_enum = raw[6];
    state.bus_flags = flags;
  }
  return state;
}

static void ps4_wait_source_transition(FILE *log, const pbs_ps4_source_state *before) {
  if (!before->mode_valid && !before->bus_valid) {
    fprintf(log, "Source transition: no safe output observer available; Start proceeds (readiness unverified)\n");
    return;
  }
  const uint64_t began = milliseconds();
  pbs_ps4_source_state last = {0};
  int have_last = 0, consecutive = 0, saw_change = 0;
  while (milliseconds() - began < 1500) {
    pbs_ps4_source_state now = ps4_sample_output_state();
    if (pbs_ps4_source_observable(before, &now) &&
        pbs_ps4_source_changed(before, &now)) saw_change = 1;
    if (saw_change && have_last &&
        pbs_ps4_source_observable(&last, &now) &&
        !pbs_ps4_source_changed(&last, &now)) ++consecutive;
    else consecutive = 0;
    last = now;
    have_last = 1;
    if (saw_change && consecutive >= 3) {
      fprintf(log, "Source transition: observed output change then stable snapshot after %llums; SCAN-IN PIXELS UNVERIFIED\n",
              (unsigned long long)(milliseconds() - began));
      return;
    }
    usleep(30000);
  }
  fprintf(log, "Source transition: 1500ms elapsed changed=%d, no proven READY_SCANIN; Start proceeds for diagnostic capture\n",
          saw_change);
}
#endif

static void ps4_packet_diagnostics(FILE *log, uint64_t index, const unsigned char *frame,
                                   int length, const unsigned char info[0x40]) {
  if (index != 0 && index != 1 && index != 59 && index != 119 && index != 299) return;
  fprintf(log, "packet[%llu] len=%d metadata=", (unsigned long long)index, length);
  for (int j = 0; j < 0x40; ++j) fprintf(log, "%02x", info[j]);
  fprintf(log, " prefix=");
  for (int j = 0; j < 24 && j < length; ++j) fprintf(log, "%02x", frame[j]);
  fputc('\n', log);
}

#ifdef PBS_PS4_CAPTURE_SECONDS
/* Keep an Annex B sample from the standalone smoke run so it can be decoded
 * offline even when no browser is attached during the short loader window. */
static void ps4_smoke_header(FILE *output, const unsigned char *sps, int sps_len,
                             const unsigned char *pps, int pps_len) {
  const unsigned char start_code[] = {0, 0, 0, 1};
  fwrite(start_code, 1, sizeof start_code, output);
  fwrite(sps, 1, (size_t)sps_len, output);
  fwrite(start_code, 1, sizeof start_code, output);
  fwrite(pps, 1, (size_t)pps_len, output);
}
#endif

int main(int argc, char **argv) {
  mkdir(PBS_PS4_DATA_ROOT, 0777); mkdir(PBS_PS4_CAPTURE_DIR, 0777);
  FILE *log = fopen(PBS_PS4_CAPTURE_DIR "/capture.txt", "w");
  if (!log) return 1;
  setvbuf(log, NULL, _IONBF, 0);
  /* Compact problem index next to the full report: on a failed run it is the
   * first file to read, and it stays complete even if the report cannot be
   * fetched. Records mirror into capture.txt as PROBLEM/SUMMARY lines. */
  char problem_context[128];
  snprintf(problem_context, sizeof problem_context,
           "report=capture.txt fw=%08x usage=%d paste=%d wait_events=%d",
           kernel_get_fw_version(), PBS_PS4_CAPTURE_USAGE, PBS_PS4_IMAGE_PASTE,
           PBS_PS4_READ_WAIT_EVENTS);
  pbs_problem_open(PBS_PS4_CAPTURE_DIR "/problems.txt", problem_context);
#ifdef PBS_PS4_CAPTURE_SECONDS
  crash_fd = fileno(log);
  struct sigaction action = {.sa_sigaction = crash_report, .sa_flags = SA_SIGINFO};
  sigemptyset(&action.sa_mask);
  sigaction(SIGSEGV, &action, NULL); sigaction(SIGBUS, &action, NULL);
  sigaction(SIGILL, &action, NULL); sigaction(SIGABRT, &action, NULL);
  fprintf(log, "payload main=%p\n", (void *)main);
#endif
  fprintf(log, "PBS PS4 startup argc=%d argv=%p\n", argc, (void *)argv);
  int height = PBS_PS4_CAPTURE_HEIGHT;
  int width = height == 1080 ? 1920 : 1280;
  int port = 9999, status = 1, initialized = 0, started = 0;
  unsigned seconds = 0;
#ifdef PBS_PS4_CAPTURE_SECONDS
  seconds = PBS_PS4_CAPTURE_SECONDS;
#endif
  /* GoldHEN inherits the host process argv (e.g. --restart-due-to-crash).
   * Interpret options only when the loader explicitly supplies avserve. */
  for (int i = argc > 1 && !strcmp(argv[1], "avserve") ? 2 : argc; i < argc; ++i) {
    if (!strcmp(argv[i], "avserve") || !strcmp(argv[i], "noaudio")) continue;
    if (!strcmp(argv[i], "720p")) { width = 1280; height = 720; continue; }
    if (!strcmp(argv[i], "1080p")) { width = 1920; height = 1080; continue; }
    const char *value = argv[i];
    int duration = !strncmp(value, "seconds=", 8);
    if (duration) value += 8;
    char *end;
    unsigned long number = strtoul(value, &end, 10);
    if (!*value || *end || (duration ? number > 60 : number < 1 || number > 65535)) {
      fprintf(log, "Invalid option: %s\n", argv[i]);
      pbs_problem_record(log, PBS_PROBLEM_FATAL, "startup", 0, 0, "Invalid option: %s", argv[i]);
      pbs_problem_finish(log, 1);
      fclose(log); return 1;
    }
    if (duration) seconds = (unsigned)number;
    else port = (int)number;
  }
  fprintf(log, "PBS PS4 fw=%08x %dx%d%d port=%d seconds=%u usage=%d paste=%d\n",
          kernel_get_fw_version(), width, height, PBS_PS4_CAPTURE_FPS, port, seconds,
          PBS_PS4_CAPTURE_USAGE, PBS_PS4_IMAGE_PASTE);
  const pbs_ps4_firmware *firmware = pbs_ps4_firmware_find(kernel_get_fw_version());
  if (!firmware) {
    fprintf(log, "PS4 kernel layout unavailable or unsupported; no capture or kernel patches attempted\n");
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "startup", 0, 0,
                       "PS4 kernel layout unavailable or unsupported (fw=%08x); "
                       "no capture or kernel patches attempted",
                       kernel_get_fw_version());
    pbs_problem_finish(log, 1);
    fclose(log);
    return 1;
  }
  fprintf(log, "Capture ABI profile: %s\n", firmware->name);
#if PBS_PS4_DYNAMIC_LAYOUT
  fprintf(log,"Runtime layout: acmgr=+%x gates=+%x,+%x (ELF/code discovery, no firmware offset table)\n",
          firmware->acmgr_offset,firmware->gates[0].offset,firmware->gates[1].offset);
#endif
  fprintf(log, "ReadVideo flags=%d (%s), native-wait-event path=%s\n",
          PBS_PS4_READ_WAIT_EVENTS ? 0 : 1,
          PBS_PS4_READ_WAIT_EVENTS ? "native wait, non-cancellable" : "nonblocking ring probe",
          PBS_PS4_READ_WAIT_EVENTS ? "OPT-IN RISK" : "disabled for responsive cleanup");
  void *handle = NULL;
  unsigned retries = 0;
#ifdef PBS_PS4_SMOKE_REOPEN_EMPTY
  int empty_source_reopened = 0;
#endif
  int server_ready = 0;
  uint64_t overall_packets = 0, overall_bytes = 0, deadline = 0;
  int recovery_failed = 0;
  unsigned char *frame = NULL;
#ifdef PBS_PS4_CAPTURE_SECONDS
  FILE *smoke = NULL;
  uint64_t smoke_bytes = 0;
#endif
  if (pbs_ps4_capture_begin(log)) {
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "ownership", 0, 0,
                       "capture begin refused: unresolved previous capture ownership/teardown");
    goto done;
  }
  ps4_static_state(log, "before init");
  int rc = pbs_ps4_init();
  fprintf(log, "Init -> %08x\n", rc);
  if (rc < 0) {
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "init", rc, 0, "Avcap Init failed before any Open");
    goto cleanup;
  }
  initialized = 1;
  ps4_static_state(log, "after remote request");
#if PBS_PS4_DAEMON_DUMP
  /* Match the successful admission snapshots: debug gates and the payload's
   * tracing identity are established by Init, before inspecting the daemon.
   * Dump before Open, while the native daemon has no capture to service. */
  fprintf(log, "Daemon dump after initialized control, before Open\n");
  int dump_rc = pbs_ps4_daemon_dump(log, firmware, PBS_PS4_CAPTURE_DIR "/daemon-dump.bin");
  if (dump_rc == -2) {
    fprintf(log, "Daemon dump detach uncertain; refusing Open\n");
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "daemon-dump", 0, 0,
                       "daemon dump detach uncertain; refusing Open");
    goto cleanup;
  }
#endif
capture_open:
  handle = NULL;
  started = 0;
  /* Native RemotePlay uses a 0x108-byte IPC request. +4 is usage 0;
   * +8/+c are encode/output profiles: 19=720p60, 20=1080p60.
   * No PS5 Avcap2 structures or resource-arbitrator IDs are reused. */
  uint32_t params[PBS_PS4_VIDEO_REQUEST_WORDS];
#if PBS_PS4_REMOTE_1080
  if (pbs_ps4_make_video_request_remote_1080(params, (unsigned)height, PBS_PS4_CAPTURE_USAGE,
      PBS_PS4_IMAGE_PASTE, PBS_PS4_CAPTURE_FPS)) {
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "request", 0, 0,
        "RemotePlay 1080p30 request rejected by ABI guard for %dp%d", height, PBS_PS4_CAPTURE_FPS);
    goto cleanup;
  }
  fprintf(log, "RemotePlay 1080p30: profiles=%u/%u request[0x30]=%u; native_source=%u Neo budget disabled\n",
      params[2],params[3],params[0x30/4],(unsigned)PBS_PS4_NATIVE_SOURCE);
#elif PBS_PS4_LOW_MEMORY
  if (pbs_ps4_make_video_request_low_memory(params, (unsigned)height, PBS_PS4_CAPTURE_USAGE,
                                 PBS_PS4_IMAGE_PASTE, PBS_PS4_CAPTURE_FPS)) {
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "request", 0, 0,
        "Low-memory 1080p30 request rejected by ABI guard for %dp%d", height, PBS_PS4_CAPTURE_FPS);
    goto cleanup;
  }
  fprintf(log, "Low-memory 1080p30: request[0x30]=%u; native 24MiB pool, Neo budget flag disabled\n", params[0x30/4]);
#else
  if (pbs_ps4_make_video_request_at_rate(params, (unsigned)height, PBS_PS4_CAPTURE_USAGE,
                                 PBS_PS4_IMAGE_PASTE, PBS_PS4_CAPTURE_FPS)) {
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "request", 0, 0,
        "video request rejected by ABI guard for %dp%d", height, PBS_PS4_CAPTURE_FPS);
    goto cleanup;
  }
#endif
  fprintf(log, "Avcap request usage=%u profile=%u bitrate_coefficients=%u,%u\n",
          params[1], params[2], params[0x40 / 4], params[0x44 / 4]);
#ifdef PBS_PS4_SMOKE_WAIT_SOURCE
  pbs_ps4_source_state before_open = ps4_sample_output_state();
#endif
  /* FW11 native RemotePlay retries Avcap's *specific* 0x80a70008
   * (temporarily unavailable) return: remoteplay.elf+0x2fb376..0x2fb394
   * uses 16-ms waits with a finite ~2016-ms budget, and +0x2fd213
   * also treats 0x08 as retryable. Previous PBS aborted the entire session
   * on the first 0x08, as observed on the physical FW11 console. Only retry
   * when OpenVideoEx returned no local handle, as native failure cleanup
   * follows +0x1175->+0x11d0 without publishing one. Other errors or a
   * returned handle remain quarantined: never risk overlapping owners. */
  const uint64_t open_began = milliseconds();
  unsigned busy_retries = 0;
  do {
    rc = pbs_ps4_open(&handle, params);
    if ((uint32_t)rc != 0x80a70008u || handle ||
        milliseconds() - open_began >= 2016u) break;
#if defined(PBS_PS4_CAPTURE_SECONDS) || defined(PBS_PS4_TEST_OPEN_SLOTS)
    if (!busy_retries) ps4_open_slot_snapshot(log, "at first OpenVideoEx 80a70008");
#endif
    ++busy_retries;
    usleep(16000);
  } while (milliseconds() - open_began < 2016u);
  if (busy_retries) {
    fprintf(log, "OpenVideoEx transient 80a70008 retries=%u elapsed_ms=%llu\n",
            busy_retries, (unsigned long long)(milliseconds() - open_began));
    pbs_problem_record(log, PBS_PROBLEM_NOTE, "open", (int)0x80a70008u, 0,
                       "transient 80a70008: %u bounded retries in %llums (native behavior)",
                       busy_retries, (unsigned long long)(milliseconds() - open_began));
  }
  fprintf(log, "OpenVideoEx profile=%u -> %08x handle=%p\n", params[2], rc, handle);
#if defined(PBS_PS4_CAPTURE_SECONDS) || defined(PBS_PS4_TEST_OPEN_SLOTS)
  if ((uint32_t)rc == 0x80a70008u)
    ps4_open_slot_snapshot(log, "at final OpenVideoEx 80a70008");
#endif
  if (rc < 0 || !handle) {
    /* libSceAvcap may reserve its capture-ID bitmap before the IPC open
     * finishes. A failed Open can poison that slot even with *out=NULL;
     * retrying blindly can create a second owner or leak the first one. */
    fprintf(log, "OpenVideoEx failure quarantined; not reopening an unknown capture slot\n");
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "open", rc, 0,
                       "OpenVideoEx profile=%u failed and is quarantined; no reopen of an unknown slot",
                       params[2]);
    handle = NULL;
    status = 1;
    goto cleanup;
  }
#if defined(PBS_PS4_CAPTURE_SECONDS) || defined(PBS_PS4_TEST_OPEN_SLOTS)
  /* Successful Open should now own an encoder resource. Read-only service
   * evidence differentiates an allocated encoder slot from a source which
   * actually carries video pixels; neither implies the other. */
  ps4_open_slot_snapshot(log, "after successful OpenVideoEx");
#endif
  ps4_static_state(log, "after OpenVideoEx");
#ifdef PBS_PS4_SMOKE_WAIT_SOURCE
  ps4_wait_source_transition(log, &before_open);
#endif
  /* Native PS4 SceRemotePlay +0x2FD8FA..+0x2FDA64 opens and waits for
   * Avcap GetInfo to supply headers before its caller invokes Start
   * (+0x2FD3A5). Old PBS called Start first, racing the encoder's staged
   * initialization. This wait is the observed native pre-Start barrier;
   * it does NOT assert that compositor scan-in contains visible pixels. */
  unsigned char sps[128], pps[128];
  int sps_len = 0, pps_len = 0;
  uint64_t until = milliseconds() + 2000;
  do {
    if (sps_len <= 0) sps_len = pbs_ps4_info(handle, 1, sps, sizeof sps);
    if (pps_len <= 0) pps_len = pbs_ps4_info(handle, 2, pps, sizeof pps);
    if (sps_len > 0 && pps_len > 0) break;
    usleep(5000);
  } while (milliseconds() < until);
  fprintf(log, "Pre-Start GetInfo SPS=%d PPS=%d elapsed_ms=%llu\n",
          sps_len, pps_len, (unsigned long long)(milliseconds() - (until - 2000)));
#ifdef PBS_PS4_OBSERVE_VIDEO_MODE
  /* Read-only state between header readiness and Start; the service owns
   * READY_SCANIN dispatch and these snapshots cannot certify its pixels. */
  ps4_static_state(log, "after Pre-Start GetInfo");
#endif
  if (sps_len <= 0 || sps_len > 128 || pps_len <= 0 || pps_len > 128 ||
      (sps[0] & 31) != 7 || (pps[0] & 31) != 8) {
    /* This path used to retry silently: the only evidence was the GetInfo line
     * above. State the exact header outcome so a failed run is diagnosed from
     * the problem index alone. */
    pbs_problem_record(log, PBS_PROBLEM_FAIL, "prestart", 0, 0,
                       "pre-Start headers invalid: SPS=%d PPS=%d nal=%02x/%02x",
                       sps_len, pps_len, sps_len > 0 ? sps[0] & 31 : 0,
                       pps_len > 0 ? pps[0] & 31 : 0);
    goto capture_retry;
  }
#if PBS_PS4_ENCODER_STATE
  int before_snapshot_rc = pbs_ps4_encoder_snapshot(log, firmware,
      PBS_PS4_CAPTURE_DIR "/encoder-before-start.bin");
  fprintf(log, "Encoder before-Start snapshot rc=%d\n", before_snapshot_rc);
  if (before_snapshot_rc == -2) {
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "encoder-snapshot", 0, 0,
                       "before-Start encoder snapshot failed; diagnostic integrity uncertain");
    goto cleanup;
  }
#endif
#if PBS_PS4_VENC_JOURNAL
  if (pbs_ps4_venc_journal_apply(log, firmware)) {
    fprintf(log, "Venc journal unavailable; refusing diagnostic Start\n");
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "venc-journal", 0, 0,
                       "venc journal unavailable; refusing diagnostic Start");
    goto cleanup;
  }
#endif
  rc = pbs_ps4_start(handle);
  fprintf(log, "Start -> %08x\n", rc);
  if (rc < 0) {
    pbs_problem_record(log, PBS_PROBLEM_FAIL, "start", rc, 0,
                       "Avcap Start failed after valid pre-Start headers");
    goto capture_retry;
  }
  started = 1;
#if PBS_PS4_ENCODER_STATE
  /* Post-Start / pre-Read snapshot: positive control for the scan-in model.
   * Offline RE of the 24/2 failure shows StartSequence sets core+0xb0a and
   * that the failing Read returns a VencCore status event with that bit
   * still set, so the pre-existing before-Start/in-flight pair cannot say
   * whether StartSequence or the first Read arms it. This point can. The
   * build guard limits this variant to the working 720p60 worker smoke, so
   * the 24/2 route keeps using rc + printf-journal evidence. Read-only
   * PBSEST02, same helper as the other two snapshots. */
  int after_start_snapshot_rc = pbs_ps4_encoder_snapshot(log, firmware,
      PBS_PS4_CAPTURE_DIR "/encoder-after-start.bin");
  fprintf(log, "Encoder after-Start snapshot rc=%d\n", after_start_snapshot_rc);
  if (after_start_snapshot_rc == -2) {
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "encoder-snapshot", 0, 0,
                       "after-Start encoder snapshot failed; diagnostic integrity uncertain");
    goto cleanup;
  }
#endif
  ps4_static_state(log, "after Start");
  if (!frame) frame = pbs_video_alloc(0x200000);
  pbs_audio_configure("none", 0, 0);
  if (!frame) {
    fprintf(log, "Frame buffer allocation failed errno=%d\n", errno);
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "memory", 0, errno,
                       "frame buffer allocation failed");
    goto capture_retry;
  }
  if (pbs_h264_server_start_at_rate(port, width, height, PBS_PS4_CAPTURE_FPS, sps, sps_len, pps, pps_len)) {
    fprintf(log, "HTTP server failed on port %d errno=%d\n", port, errno);
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "http", 0, errno,
                       "HTTP server failed on port %d", port);
    goto capture_retry;
  }
  /* A second client may POST /capture/restart while Open/GetInfo/Start is
   * already servicing the first request. The new encoder generation fulfills
   * both requests; carrying the stale flag into the fresh Read loop would
   * trigger an unnecessary third Open and consume recovery budget. Keep the
   * coalescing local to PS4's own reopen path; PS5 retains its behavior. */
  if (retries && pbs_h264_take_restart_request())
    fprintf(log, "Coalesced duplicate HTTP restart received during Avcap reopen\n");
  server_ready = 1;
  if (!deadline && seconds) deadline = milliseconds() + seconds * 1000ULL;
#ifdef PBS_PS4_CAPTURE_SECONDS
  if (!overall_packets) {
    smoke = fopen(PBS_PS4_CAPTURE_DIR "/smoke.h264", "wb");
    if (smoke) {
      ps4_smoke_header(smoke, sps, sps_len, pps, pps_len);
      if (ferror(smoke)) { fclose(smoke); smoke = NULL; }
    }
    fprintf(log, "Smoke Annex B recording %s\n", smoke ? "enabled" : "unavailable");
  } else if (smoke) {
    /* Start each new encoder sequence with its own SPS/PPS for offline decode. */
    ps4_smoke_header(smoke, sps, sps_len, pps, pps_len);
  }
#endif
  uint64_t began = milliseconds(), last_frame = began, count = 0, total = 0;
  uint64_t next_source_check = began + 2000;
  int needs_restart = 0;
  int publish_failed = 0;
  int inflight_shot = 0;
  size_t min_packet = SIZE_MAX, max_packet = 0;
  while (pbs_h264_server_running() && (!deadline || milliseconds() < deadline)) {
    if (milliseconds() >= next_source_check) {
#if !PBS_PS4_REMOTE_AVCAP
      ps4_static_state(log, "during ReadVideo");
#endif
      next_source_check = milliseconds() + 2000;
    }
    if (pbs_h264_take_keyframe_request()) {
      fprintf(log, "Keyframe request\n");
      int key_rc = pbs_ps4_keyframe(handle);
      fprintf(log, "Keyframe -> %08x\n", key_rc);
      if (key_rc < 0)
        pbs_problem_record(log, PBS_PROBLEM_FAIL, "keyframe", key_rc, 0,
                           "requested IDR was refused by Avcap");
    }
    unsigned char info[0x40] = {0};
    /* FW11 native RP uses flags=0; libSceAvcap+0x1500 retries its wait on
     * EVERY positive event wake, so an empty ring with repeated wakes may
     * never return. Select+0x1802 has the same unbounded notification loop,
     * and no safe cancel ABI was found. flags=1 jumps directly to the ring
     * read (+0x1551), giving PBS bounded control for stop/restart/cleanup.
     * Retain flags=0 only as an explicitly selected comparison experiment;
     * native Stop/Close are never issued concurrently with ReadVideo. */
    rc = pbs_ps4_read(handle, frame, 0x200000, info,
                      PBS_PS4_READ_WAIT_EVENTS ? 0 : 1);
#if PBS_PS4_REMOTE_AVCAP
    if ((uint32_t)rc == 0x80a70004u) {
      /* A producer overrun invalidates inter-frame references. The direct
       * reader has already resynchronized its cursor; request a new IDR and
       * make both recording and clients wait for the new generation. */
      fprintf(log, "Avcap ring overrun; resynchronized, requesting IDR\n");
      pbs_problem_record(log, PBS_PROBLEM_WARN, "read", (int)0x80a70004u, 0,
                         "ring overrun at packet %llu; reader resynchronized and requested IDR",
                         (unsigned long long)overall_packets);
      pbs_h264_discontinuity();
      if (pbs_ps4_keyframe(handle) < 0) {
        pbs_problem_record(log, PBS_PROBLEM_FATAL, "keyframe", 0, 0,
                           "IDR request after ring overrun failed; capture cannot continue");
        publish_failed = 1;
        break;
      }
      continue;
    }
    if (rc < 0) {
      fprintf(log, "Remote ReadVideo failed rc=%08x; ending capture before further target calls\n",
              (unsigned)rc);
      pbs_problem_record(log, PBS_PROBLEM_FATAL, "read", rc, 0,
                         "Remote ReadVideo failed after %llu packets; capture ended",
                         (unsigned long long)overall_packets);
#if PBS_PS4_ENCODER_STATE
      int snapshot_rc = pbs_ps4_encoder_snapshot(log, firmware,
          PBS_PS4_CAPTURE_DIR "/encoder-state.bin");
      fprintf(log, "Encoder failure snapshot rc=%d\n", snapshot_rc);
#endif
      publish_failed = 1;
      break;
    }
#endif
    if (rc > 0 && rc <= 0x200000) {
#if PBS_PS4_ENCODER_STATE
      if (!inflight_shot) {
        inflight_shot = 1;
        int inflight_rc = pbs_ps4_encoder_snapshot(log, firmware,
            PBS_PS4_CAPTURE_DIR "/encoder-inflight.bin");
        fprintf(log, "Encoder in-flight snapshot rc=%d packets=%llu\n",
                inflight_rc, (unsigned long long)overall_packets);
      }
#endif
      ps4_packet_diagnostics(log, overall_packets, frame, rc, info);
      if ((size_t)rc < min_packet) min_packet = (size_t)rc;
      if ((size_t)rc > max_packet) max_packet = (size_t)rc;
#ifdef PBS_PS4_CAPTURE_SECONDS
      /* Bound the diagnostic recording to 32 MiB even with atypical encoder
       * output, while keeping the HTTP stream running for the full test. */
      if (smoke) {
        if (smoke_bytes + (uint64_t)rc > 32u * 1024u * 1024u) {
          /* Stop at the first omitted AU. Appending smaller P-frames later
           * would leave references to missing frames in the diagnostic file. */
          if (fclose(smoke)) fprintf(log, "Smoke recording flush failed\n");
          smoke = NULL;
          fprintf(log, "Smoke recording capped at an access-unit boundary\n");
          fprintf(log, "Smoke Annex B recorded %llu bytes\n",
                  (unsigned long long)smoke_bytes);
        } else if (fwrite(frame, 1, (size_t)rc, smoke) == (size_t)rc) {
          smoke_bytes += (uint64_t)rc;
        } else { fclose(smoke); smoke = NULL; fprintf(log, "Smoke recording write failed\n"); }
      }
#endif
      if (pbs_h264_publish(frame, rc, pbs_ps4_pts90k_at_rate(overall_packets, PBS_PS4_CAPTURE_FPS))) {
        fprintf(log, "H264 publish failed after %llu frames; stopping capture (no false streaming success)\n",
                (unsigned long long)overall_packets);
        pbs_problem_record(log, PBS_PROBLEM_FATAL, "publish", 0, 0,
                           "H264 publish failed after %llu frames", (unsigned long long)overall_packets);
        publish_failed = 1;
        break;
      }
      ++count; total += rc; last_frame = milliseconds();
      ++overall_packets; overall_bytes += rc;
    } else {
      if (milliseconds() - last_frame > 3000) {
        fprintf(log, "ReadVideo stalled rc=%08x errno=%d\n", rc, errno);
        pbs_problem_record(log, PBS_PROBLEM_FAIL, "read", rc, errno,
                           "ReadVideo stalled >3000ms after %llu packets; requesting recovery",
                           (unsigned long long)overall_packets);
        needs_restart = 1;
        break;
      }
      usleep(1000);
    }
    if (pbs_h264_take_restart_request()) {
      fprintf(log, "Restart requested by HTTP client\n");
      needs_restart = 1;
      break;
    }
#ifdef PBS_PS4_SMOKE_REOPEN_EMPTY
    /* Diagnostic-only recovery experiment. Real black Avcap samples are
     * ~44 bytes/frame; after 3s of consistently tiny frames, close and
     * reopen ONCE while preserving the owning RemotePlay request. A static
     * menu can compress very well, so this must not affect production. */
    if (!empty_source_reopened && milliseconds() - began >= 3000 &&
        count >= 90 && total / count < 128) {
      fprintf(log, "SUSPECTED EMPTY SCAN-IN: %llu packets %llu bytes, requesting one diagnostic Avcap reopen\n",
              (unsigned long long)count, (unsigned long long)total);
      pbs_problem_record(log, PBS_PROBLEM_WARN, "source", 0, 0,
                         "suspected empty scan-in: %llu packets %llu bytes in %llums",
                         (unsigned long long)count, (unsigned long long)total,
                         (unsigned long long)(milliseconds() - began));
      empty_source_reopened = 1;
      needs_restart = 1;
      break;
    }
#endif
  }
  uint64_t elapsed_ms = milliseconds() - began;
  double fps = elapsed_ms ? (double)count * 1000.0 / (double)elapsed_ms : 0;
  double mbps = elapsed_ms ? (double)total * 8.0 / (double)elapsed_ms / 1000.0 : 0;
  fprintf(log, "session packets=%llu bytes=%llu min=%zu max=%zu elapsed_ms=%llu fps=%.2f mbps=%.5f\n",
    (unsigned long long)count, (unsigned long long)total, count ? min_packet : 0,
    max_packet, (unsigned long long)elapsed_ms, fps, mbps);
  if (elapsed_ms >= 2000 && count >= 60 && mbps < 0.10) {
    fprintf(log, "WARNING: unusually low H.264 output rate; inspect decoded frames, source/scan-in unverified\n");
    pbs_problem_record(log, PBS_PROBLEM_WARN, "stream", 0, 0,
                       "unusually low H.264 output: %llu packets %.5f mbps in %llums",
                       (unsigned long long)count, mbps, (unsigned long long)elapsed_ms);
  }
  if (overall_packets && !publish_failed) status = 0;
  /* The payload cannot decode the hardware stream. This status only proves
   * packets were delivered to PBS; actual visible pixels require external
   * ffmpeg decoding and the smoke verifier. */
  fprintf(log, "transport_packets=%llu video_pixels=UNVERIFIED_NEEDS_FFMPEG publish_failed=%d\n",
          (unsigned long long)overall_packets, publish_failed);
  if (publish_failed) { status = 1; goto cleanup; }
  if (!needs_restart) goto cleanup;
capture_retry:
  if (started) {
    int stop_rc = pbs_ps4_stop(handle);
    fprintf(log, "Stop -> %08x\n", stop_rc);
    if (stop_rc < 0) {
      pbs_problem_record(log, PBS_PROBLEM_FAIL, "stop", stop_rc, 0, "Avcap Stop failed during recovery");
      recovery_failed = 1;
    }
    started = 0;
    ps4_static_state(log, "after Stop");
  }
  if (handle) {
    int close_rc = pbs_ps4_close(handle);
    fprintf(log, "Close -> %08x\n", close_rc);
#if defined(PBS_PS4_CAPTURE_SECONDS) || defined(PBS_PS4_TEST_OPEN_SLOTS)
    ps4_open_slot_snapshot(log, "after retry Close");
#endif
    if (close_rc < 0) {
      pbs_problem_record(log, PBS_PROBLEM_FAIL, "close", close_rc, 0, "Avcap Close failed during recovery");
      recovery_failed = 1;
    }
    handle = NULL;
  }
  /* Close invalidates the local Avcap handle even if its service teardown
   * fails (libSceAvcap +0x1762), so a failed Stop/Close cannot safely be
   * followed by another Open. Report failure and release the RP request. */
  if (recovery_failed) {
    fprintf(log, "Avcap cleanup failed; refusing to reopen a potentially active resource\n");
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "cleanup", 0, 0,
                       "Avcap cleanup failed; refusing to reopen a potentially active resource");
    status = 1;
    goto cleanup;
  }
  if ((server_ready && !pbs_h264_server_running()) || retries >= 3 ||
      (deadline && milliseconds() >= deadline)) {
    status = 1;
    fprintf(log, "Avcap recovery incomplete after %u attempts\n", retries);
    pbs_problem_record(log, PBS_PROBLEM_FATAL, "recovery", 0, 0,
                       "Avcap recovery incomplete after %u attempts", retries);
    goto cleanup;
  }
  ++retries;
  fprintf(log, "Avcap reopen attempt %u/3\n", retries);
  pbs_h264_discontinuity();
  usleep(150000);
  goto capture_open;
cleanup:
  fprintf(log, "total packets=%llu bytes=%llu reopen=%u\n",
          (unsigned long long)overall_packets, (unsigned long long)overall_bytes, retries);
#ifdef PBS_PS4_CAPTURE_SECONDS
  if (smoke) {
    if (fclose(smoke)) fprintf(log, "Smoke recording flush failed\n");
    fprintf(log, "Smoke Annex B recorded %llu bytes\n", (unsigned long long)smoke_bytes);
  }
#endif
  pbs_h264_server_stop();
  pbs_h264_server_wait();
  pbs_video_free(frame, 0x200000);
#if PBS_PS4_VENC_JOURNAL
  if (pbs_ps4_venc_journal_pending()) {
    int journal_rc = pbs_ps4_venc_journal_finish(log,
        PBS_PS4_CAPTURE_DIR "/venc-journal.bin");
    if (journal_rc) {
      fprintf(log, "Venc journal finish failed rc=%d\n", journal_rc);
      pbs_problem_record(log, PBS_PROBLEM_FAIL, "venc-journal", journal_rc, 0,
                         "venc journal finish failed");
      status = 1;
    }
    if (pbs_ps4_venc_journal_pending()) {
      fprintf(log, "Venc journal ownership uncertain; capture quarantined, reboot required\n");
      pbs_problem_record(log, PBS_PROBLEM_FATAL, "venc-journal", 0, 0,
                         "venc journal ownership uncertain; capture quarantined, reboot required");
      status = 1;
      goto done;
    }
  }
#endif
  if (started) {
    int stop_rc = pbs_ps4_stop(handle);
    fprintf(log, "Stop -> %08x\n", stop_rc);
    if (stop_rc < 0) {
      pbs_problem_record(log, PBS_PROBLEM_FAIL, "stop", stop_rc, 0, "Avcap Stop failed at teardown");
      status = 1;
    }
  }
  if (handle) {
    int close_rc = pbs_ps4_close(handle);
    fprintf(log, "Close -> %08x\n", close_rc);
#if defined(PBS_PS4_CAPTURE_SECONDS) || defined(PBS_PS4_TEST_OPEN_SLOTS)
    ps4_open_slot_snapshot(log, "after final Close");
#endif
    if (close_rc < 0) {
      pbs_problem_record(log, PBS_PROBLEM_FAIL, "close", close_rc, 0, "Avcap Close failed at teardown");
      status = 1;
    }
  }
  if (initialized) {
    ps4_static_state(log, "before term");
    int term_rc = pbs_ps4_term();
    fprintf(log, "Term -> %08x\n", term_rc);
    if (term_rc < 0) {
      pbs_problem_record(log, PBS_PROBLEM_FAIL, "term", term_rc, 0, "Avcap Term failed at teardown");
      status = 1;
    }
  }
  pbs_ps4_capture_end();
done:
  fprintf(log, "exit=%d\n", status);
  /* The SUMMARY names the first problem (usually the root cause) and is the
   * last thing a reader needs from the compact index. */
  pbs_problem_finish(log, status);
  fclose(log);
  return status;
}



