// SPDX-License-Identifier: GPL-3.0-or-later
/* PBS streaming payload. Research commands remain in the legacy repository. */
#define PBS_DATA_DIR "/data/pbs"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ps5/kernel.h>



#include "common/http/server.h"
#include "common/process/inject.h"



typedef struct {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t, void *, uint32_t *);

static int notify(const char *text) {
  notify_request_t req;
  memset(&req, 0, sizeof req);
  strncpy(req.message, text, sizeof req.message - 1);
  return sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

static FILE *open_report(const char *name, const char *mode) {
  char path[160];
  snprintf(path, sizeof path, "%s/%s", PBS_DATA_DIR, name);
  FILE *f = fopen(path, mode);
  if (f) setvbuf(f, NULL, _IONBF, 0);
  return f;
}

static unsigned char scratch_zero[4096];
static uint32_t requested_video_bitrate;
static uint32_t requested_video_budget;
static int native_rate_config;
static uint32_t requested_min_qp = 35;
static unsigned stress_keyframe_period;
static int stress_live;
static int live_request;
static unsigned live_start_attempts;
static int live_server_started;
static int bitrate_after_start;

static const char *fmt_rc(long rc);

static void set_video_bitrate(FILE *f, pbs_inject_t *ctx, uint64_t handle) {
  intptr_t fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2SetVideoBitrate");
  unsigned char params[16] = {0};
  uint64_t size = sizeof params;
  memcpy(params, &size, 8); memcpy(params + 8, &requested_video_bitrate, 4);
  memcpy(params + 12, &requested_video_budget, 4);
  if (requested_video_budget) fprintf(f, "SetVideoBitrate companion=%u\n", requested_video_budget);
  if (fn && !pbs_inject_write_at(ctx, ctx->scratch + 0x780, params, sizeof params))
    fprintf(f, "SetVideoBitrate(%u%s) -> %s\n", requested_video_bitrate,
      bitrate_after_start ? " after Start" : "", fmt_rc(pbs_inject_call_fast(ctx, fn,
      (uint64_t[6]){handle, (uint64_t)ctx->scratch + 0x780, 0, 0, 0, 0})));
}

static void param_u64(unsigned char *p, size_t offset, uint64_t value) {
  memcpy(p + offset, &value, sizeof value);
}

static void param_u32(unsigned char *p, size_t offset, uint32_t value) {
  memcpy(p + offset, &value, sizeof value);
}

static const char *fmt_rc(long rc) {
  static char buf[64];
  if (rc == LONG_MIN) {
    snprintf(buf, sizeof buf, "NO RESPONSE");
  } else {
    snprintf(buf, sizeof buf, "0x%08lx%s", (unsigned long)rc,
             ((int32_t)(uint32_t)rc >= 0 ? " (ok)" : " (error)"));
  }
  return buf;
}

static void dump_scratch(FILE *f, pbs_inject_t *ctx, size_t len) {
  unsigned char buf[4096];
  if (len > sizeof buf) len = sizeof buf;
  if (pbs_inject_read(ctx, buf, len)) {
    fprintf(f, "  scratch: read failed errno=%d\n", errno);
    return;
  }
  fprintf(f, "  scratch: ");
  for (size_t i = 0; i < len; i++) fprintf(f, "%02x", buf[i]);
  fprintf(f, "\n");
  fflush(f);
}

static uint64_t capture_clock_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

typedef struct {
  uint64_t handle;
  intptr_t read_fn, stop_fn, close_fn, alloc_fn, free_fn;
  intptr_t buffer;
  FILE *raw, *info;
  uint64_t packets, bytes;
  int publish; /* Publish audio blocks to the HTTP server instead of a file. */
  int analyze; /* Accumulate float32 sample statistics. */
  uint32_t last_pts;
  uint32_t vol_original;
  intptr_t setvol_fn;
  int vol_changed;
  double sumsq;
  uint64_t floats;
  float peak;
} pbs_audio_t;

/* Open the audio channel with the verified 0x38-byte parameter structure: common size 0x18, usage 2, format, arbitrator feature and 128 kbit/s rate. */
static int pbs_audio_open(FILE *f, pbs_inject_t *ctx, pbs_audio_t *audio, int width, uint32_t format) {
  memset(audio, 0, sizeof *audio);
  intptr_t open_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2OpenAudio");
  intptr_t start_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Start");
  audio->read_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2ReadAudio");
  audio->stop_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Stop");
  audio->close_fn = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Close");
  audio->alloc_fn = pbs_resolve_in(ctx->pid, "libSceLibcInternal.sprx", "malloc");
  audio->free_fn = pbs_resolve_in(ctx->pid, "libSceLibcInternal.sprx", "free");
  if (!open_fn || !start_fn || !audio->read_fn || !audio->alloc_fn) return -1;
  unsigned char params[0x440] = {0};
  param_u64(params, 0x408, 0x38);
  param_u64(params, 0x410, 0x18);
  param_u32(params, 0x41c, 2);
  param_u32(params, 0x420, format);
  param_u32(params, 0x424, width == 2560 ? 11 : 12);
  param_u64(params, 0x428, 0x10);
  param_u32(params, 0x430, 128000);
  if (pbs_inject_write(ctx, params, sizeof params)) return -1;
  long rc = pbs_inject_call_fast(ctx, open_fn, (uint64_t[6]){
      (uint64_t)ctx->scratch + 0x400, (uint64_t)ctx->scratch + 0x408, 0, 0, 0, 0});
  fprintf(f, "Audio OpenAudio(format=%u) -> %s%s\n", format, fmt_rc(rc), ctx->asserted ? " [ASSERT]" : "");
  if (rc == LONG_MIN || (int32_t)(uint32_t)rc < 0 || ctx->asserted) return -1;
  if (pbs_inject_read_at(ctx, ctx->scratch + 0x400, &audio->handle, sizeof audio->handle) || !audio->handle) {
    fprintf(f, "Audio handle mancante\n");
    return -1;
  }
  audio->buffer = (intptr_t)pbs_inject_call_fast(ctx, audio->alloc_fn, (uint64_t[6]){65536, 0, 0, 0, 0, 0});
  if (audio->buffer <= 0) return -1;
  rc = pbs_inject_call_fast(ctx, start_fn, (uint64_t[6]){audio->handle, 0, 0, 0, 0, 0});
  fprintf(f, "Audio Start -> %s%s\n", fmt_rc(rc), ctx->asserted ? " [ASSERT]" : "");
  if (rc == LONG_MIN || (int32_t)(uint32_t)rc < 0) return -1;
  return 0;
}

static void pbs_audio_close(FILE *f, pbs_inject_t *ctx, pbs_audio_t *audio) {
  if (audio->vol_changed && audio->setvol_fn && audio->handle && !ctx->asserted) {
    float original;
    memcpy(&original, &audio->vol_original, sizeof original);
    fprintf(f, "Audio SetAudioOutVolume(restore %.6f) -> %s\n", (double)original,
            fmt_rc(pbs_inject_call_fast_fp(ctx, audio->setvol_fn, (uint64_t[6]){audio->handle, 0, 0, 0, 0, 0}, original)));
    audio->vol_changed = 0;
  }
  if (audio->handle && !ctx->asserted) {
    if (audio->stop_fn) fprintf(f, "Audio Stop -> %s\n", fmt_rc(pbs_inject_call_fast(ctx, audio->stop_fn, (uint64_t[6]){audio->handle, 0, 0, 0, 0, 0})));
    if (audio->close_fn) fprintf(f, "Audio Close -> %s\n", fmt_rc(pbs_inject_call_fast(ctx, audio->close_fn, (uint64_t[6]){audio->handle, 0, 0, 0, 0, 0})));
  }
  if (audio->buffer > 0 && !ctx->asserted && audio->free_fn)
    pbs_inject_call_fast(ctx, audio->free_fn, (uint64_t[6]){(uint64_t)audio->buffer, 0, 0, 0, 0, 0});
  if (audio->raw) fclose(audio->raw);
  if (audio->info) fclose(audio->info);
  if (audio->analyze && audio->floats)
    fprintf(f, "Audio analisi: rms=%.6f picco=%.6f campioni=%llu\n",
            sqrt(audio->sumsq / (double)audio->floats), (double)audio->peak,
            (unsigned long long)audio->floats);
  fprintf(f, "Audio captured packets=%llu bytes=%llu\n",
          (unsigned long long)audio->packets, (unsigned long long)audio->bytes);
  memset(audio, 0, sizeof *audio);
}

/* Drain at most rounds available audio packets into the reusable local frame buffer. */
static void pbs_audio_drain(FILE *f, pbs_inject_t *ctx, pbs_audio_t *audio, unsigned char *frame, int rounds) {
  for (int i = 0; i < rounds && audio->read_fn; i++) {
    long rc = pbs_inject_call_fast(ctx, audio->read_fn, (uint64_t[6]){audio->handle,
        (uint64_t)audio->buffer, 65536, (uint64_t)ctx->scratch + 0x500, 1, 0});
    if (rc == LONG_MIN) { fprintf(f, "Audio ReadAudio timeout\n"); break; }
    int32_t count = (int32_t)(uint32_t)rc;
    if (count <= 0) {
      if (!audio->packets) fprintf(f, "Audio ReadAudio -> %s\n", fmt_rc(rc));
      break;
    }
    if (count > 65536 || pbs_inject_read_at(ctx, audio->buffer, frame, count)) break;
    unsigned char info[16] = {0};
    pbs_inject_read_at(ctx, ctx->scratch + 0x500, info, sizeof info);
    uint32_t pts = 0;
    memcpy(&pts, info + 8, sizeof pts);
    audio->last_pts = pts;
    if (audio->publish) {
      if (pbs_audio_publish(frame, (size_t)count, pts)) break;
    } else {
      if (audio->info) { uint32_t size = (uint32_t)count; fwrite(&size, 1, 4, audio->info); fwrite(info, 1, 16, audio->info); }
      if (audio->raw) fwrite(frame, 1, count, audio->raw);
    }
    if (audio->analyze && (count & 3) == 0) {
      const float *samples = (const float *)frame;
      for (int j = 0; j < count / 4; j++) {
        double v = samples[j];
        audio->sumsq += v * v;
        float a = v < 0 ? (float)-v : (float)v;
        if (a > audio->peak) audio->peak = a;
      }
      audio->floats += (uint64_t)(count / 4);
    }
    if (!audio->packets) {
      fprintf(f, "Audio first packet bytes=%d prefix=", count);
      for (unsigned j = 0; j < 24 && j < (unsigned)count; j++) fprintf(f, "%02x", frame[j]);
      fprintf(f, " metadata=");
      for (unsigned j = 0; j < sizeof info; j++) fprintf(f, "%02x", info[j]);
      fprintf(f, "\n");
    }
    audio->packets++; audio->bytes += (uint64_t)count;
  }
}

static int capture_stream(FILE *f, pbs_inject_t *ctx, uint64_t handle, int seconds, int port, int width, int height, int audio_mode) {
  if (seconds < 0 || seconds > 60) return -1;
  int serving = seconds == 0 || stress_live;
  int recover = 0;
  intptr_t start = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Start");
  intptr_t stop = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2Stop");
  intptr_t read = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2ReadVideo");
  intptr_t alloc = pbs_resolve_in(ctx->pid, "libSceLibcInternal.sprx", "malloc");
  intptr_t release = pbs_resolve_in(ctx->pid, "libSceLibcInternal.sprx", "free");
  intptr_t request_key = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2RequestKeyFrame");
  if (!start || !stop || !read || !alloc || !release) return -1;
  long buffer = pbs_inject_call_fast(ctx, alloc, (uint64_t[6]){0x200000, 0, 0, 0, 0, 0});
  if (buffer <= 0) return -1;
  unsigned char *frame = malloc(0x200000);
  if (!frame) goto capture_free;
  FILE *out = fopen("/data/pbs/h264.stream", "wb");
  if (!out) goto capture_free;
  if (requested_video_bitrate && !bitrate_after_start) set_video_bitrate(f, ctx, handle);
  pbs_audio_t audio;
  memset(&audio, 0, sizeof audio);
  if (audio_mode == 2 && !pbs_audio_open(f, ctx, &audio, width, 0)) audio.publish = 1;
  long rc = pbs_inject_call_fast(ctx, start, (uint64_t[6]){handle, 0, 0, 0, 0, 0});
  fprintf(f, "Start() -> %s\n", fmt_rc(rc));
  if (serving && native_rate_config && requested_video_bitrate > 30000000 &&
      rc != LONG_MIN && (int32_t)(uint32_t)rc < 0 && !ctx->asserted) {
    /* Close this caller-owned handle before the outer loop retries the
     * conservative profile. Never retry a failed remote call/assertion. */
    fprintf(f, "High bitrate Start rejected; next capture uses 30M QP=35..51\n");
    requested_video_bitrate = requested_video_budget = 30000000;
    requested_min_qp = 35;
  }
  uint64_t total = 0, frames = 0;
  uint64_t measurement_start = 0, measurement_end = 0;
  uint64_t last_stress_key_frame = UINT64_MAX;
  if (serving && rc != LONG_MIN && (int32_t)(uint32_t)rc < 0 && pbs_h264_server_running()) recover = 1;
  if (rc != LONG_MIN && (int32_t)(uint32_t)rc >= 0) {
    if (requested_video_bitrate && bitrate_after_start) set_video_bitrate(f, ctx, handle);
    intptr_t headers = pbs_resolve_in(ctx->pid, "libSceAvcap2.sprx", "sceAvcap2GetNonVclNalUnitStream");
    if (headers) {
      long hr = pbs_inject_call_fast(ctx, headers, (uint64_t[6]){handle, (uint64_t)buffer, 0, 0, 0, 0});
      fprintf(f, "GetNonVclNalUnitStream() -> %s\n", fmt_rc(hr));
      unsigned char metadata[0x1a8];
      if (hr != LONG_MIN && (int32_t)(uint32_t)hr >= 0 &&
          !pbs_inject_read_at(ctx, buffer, metadata, sizeof metadata)) {
        FILE *hf = fopen("/data/pbs/h264.headers", "wb");
        if (hf) { fwrite(metadata, 1, sizeof metadata, hf); fclose(hf); }
        uint64_t sps_len, pps_len;
        memcpy(&sps_len, metadata + 0x10, 8);
        memcpy(&pps_len, metadata + 0x98, 8);
        if (sps_len > 0 && sps_len <= 128 && pps_len > 0 && pps_len <= 128 &&
            (metadata[0x18] & 31) == 7 && (metadata[0xa0] & 31) == 8) {
          const unsigned char annexb[4] = {0, 0, 0, 1};
          fwrite(annexb, 1, 4, out);
          fwrite(metadata + 0x18, 1, sps_len, out);
          fwrite(annexb, 1, 4, out);
          fwrite(metadata + 0xa0, 1, pps_len, out);
          fprintf(f, "Annex B headers: SPS=%llu PPS=%llu\n",
                  (unsigned long long)sps_len, (unsigned long long)pps_len);
          if (serving) {
            int server_rc = pbs_h264_server_start(port, width, height, metadata + 0x18, sps_len, metadata + 0xa0, pps_len);
            fprintf(f, "H.264 HTTP :%d -> %d\n", port, server_rc);
            if (!server_rc) {
              live_server_started = 1;
              if (audio.publish) pbs_audio_configure("pcm_f32le", 2, 48000);
              notify("PBS: live H.264 ready - open the console IP in a browser");
            }
          }
        } else fprintf(f, "unexpected SPS/PPS metadata layout\n");
      }
    }
    uint64_t until = capture_clock_ms() + (uint64_t)seconds * 1000;
    uint64_t last_video_ms = capture_clock_ms();
    int reports = 0;
    measurement_start = capture_clock_ms();
    while ((serving ? pbs_h264_server_running() && (!stress_live || capture_clock_ms() < until)
                    : capture_clock_ms() < until) && !ctx->asserted) {
      if (serving && pbs_h264_take_restart_request()) { recover = 1; fprintf(f, "capture restart requested\n"); break; }
      int key_requested = serving && pbs_h264_take_keyframe_request();
      int stress_key = (!serving || stress_live) && stress_keyframe_period &&
                      frames % stress_keyframe_period == 0 && frames != last_stress_key_frame;
      if (request_key && (key_requested || stress_key)) {
        if (stress_key) last_stress_key_frame = frames;
        long kr = pbs_inject_call_fast(ctx, request_key, (uint64_t[6]){handle, 0, 0, 0, 0, 0});
        if (key_requested || kr == LONG_MIN || (int32_t)(uint32_t)kr < 0)
          fprintf(f, "RequestKeyFrame() -> %s%s\n", fmt_rc(kr), ctx->asserted ? " [ASSERT]" : "");
        if (kr == LONG_MIN || ctx->asserted) break;
      }
      rc = pbs_inject_call_fast(ctx, read, (uint64_t[6]){handle, (uint64_t)buffer,
          0x200000, (uint64_t)ctx->scratch + 0x300, 0, 0});
      if (rc == LONG_MIN) {
        fprintf(f, "ReadVideo timeout\n");
        recover = serving && !ctx->asserted;
        break;
      }
      int32_t bytes = (int32_t)(uint32_t)rc;
      if (bytes > 0 && bytes <= 0x200000) {
        if (pbs_inject_read_at(ctx, buffer, frame, bytes)) {
          fprintf(f, "frame memory read failed errno=%d\n", errno);
          break;
        }
        if (serving) {
          if (pbs_h264_publish(frame, bytes, (uint32_t)(frames * 1500))) {
            fprintf(f, "frame publish failed\n");
            break;
          }
        } else if (fwrite(frame, 1, bytes, out) != (size_t)bytes) break;
        total += bytes;
        frames++;
        last_video_ms = capture_clock_ms();
      } else if (reports++ < 3) {
        fprintf(f, "ReadVideo() -> %s\n", fmt_rc(rc));
        dump_scratch(f, ctx, 16);
      }
      /* ReadVideo blocks until a frame is available; no extra delay after
       * a successful read. Back off only on an empty/error result. */
      if (bytes <= 0) usleep(1000);
      if (serving && capture_clock_ms() - last_video_ms >= 2000) {
        fprintf(f, "capture stalled: last ReadVideo=%s; reopening session\n", fmt_rc(rc));
        recover = 1;
        break;
      }
      if (audio.handle && !ctx->asserted) pbs_audio_drain(f, ctx, &audio, frame, 2);
    }
    if (serving) fprintf(f, "live loop exit running=%d asserted=%d\n", pbs_h264_server_running(), ctx->asserted);
    long sr = pbs_inject_call_fast(ctx, stop, (uint64_t[6]){handle, 0, 0, 0, 0, 0});
    measurement_end = capture_clock_ms();
    fprintf(f, "Stop() -> %s\n", fmt_rc(sr));
  }
  if (serving) {
    if (recover) pbs_h264_discontinuity();
    else pbs_h264_server_stop();
  }
  if (audio.handle) pbs_audio_close(f, ctx, &audio);
  if (measurement_start && measurement_end > measurement_start) {
    double elapsed = (measurement_end - measurement_start) / 1000.0;
    fprintf(f, "capture metrics seconds=%.3f fps=%.3f Mbps=%.3f stress_keyframe_period=%u\n",
            elapsed, frames / elapsed, total * 8 / elapsed / 1000000, stress_keyframe_period);
  }
  fprintf(f, "capture-fast: %llu reads, %llu bytes -> /data/pbs/h264.stream\n",
          (unsigned long long)frames, (unsigned long long)total);
  fclose(out);
capture_free:
  free(frame);
  pbs_inject_call_fast(ctx, release, (uint64_t[6]){(uint64_t)buffer, 0, 0, 0, 0, 0});
  return recover;
}

/* Production entry point: exported capture APIs and caller-owned handles. */
int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "avserve";
  if (strcmp(mode, "avserve")) { notify("PBS: use avserve [720p|1080p|1440p] [bitrate=N] [minqp=N] [norate] [noaudio] [port]"); return 1; }
  char *serve_args[] = {argv[0], "avopenfast", "SceRemotePlay", "grant", "11", "2", "0", "8080", "2560", "1440", "1"};
  int audio_mode = 2;
  if (!strcmp(mode, "avserve")) {
    live_request = 1;
    native_rate_config = 1;
#ifdef PBS_PS5_NO_NATIVE_RATE_DEFAULT
    native_rate_config = 0;
#endif
    requested_min_qp = 18;
    requested_video_bitrate = 60000000;
    for (int i = 2; i < argc; i++) {
      if (!strncmp(argv[i], "bitrate=", 8)) {
        char *end = NULL;
        unsigned long rate = strtoul(argv[i] + 8, &end, 10);
        if (!end || *end || rate < 1000000 || rate > 100000000) return 1;
        requested_video_bitrate = (uint32_t)rate;
      } else if (!strncmp(argv[i], "minqp=", 6)) {
        char *end = NULL;
        unsigned long qp = strtoul(argv[i] + 6, &end, 10);
        if (!end || *end || qp < 18 || qp > 35) return 1;
        requested_min_qp = (uint32_t)qp;
      } else if (!strcmp(argv[i], "norate")) {
        native_rate_config = 0;
      }
    }
    requested_video_budget = requested_video_bitrate;
    audio_mode = 2; /* Live PCM audio for connected viewers. */
    for (int i = 2; i < argc; i++) {
      if (!strncmp(argv[i], "bitrate=", 8)) continue;
      if (!strncmp(argv[i], "minqp=", 6)) continue;
      if (!strcmp(argv[i], "norate")) continue;
      if (!strcmp(argv[i], "noaudio")) { audio_mode = 0; continue; }
      if (!strcmp(argv[i], "720p")) { serve_args[4] = "12"; serve_args[8] = "1280"; serve_args[9] = "720"; continue; }
      if (!strcmp(argv[i], "1080p")) { serve_args[4] = "12"; serve_args[8] = "1920"; serve_args[9] = "1080"; continue; }
      if (!strcmp(argv[i], "1440p") || !strcmp(argv[i], "2k")) { serve_args[4] = "11"; serve_args[8] = "2560"; serve_args[9] = "1440"; continue; }
      char *end = NULL;
      unsigned long port = strtoul(argv[i], &end, 10);
      if (!end || *end || port < 1 || port > 65535) return 1;
      serve_args[7] = argv[i];
    }
    argv = serve_args; argc = 10; mode = "avopenfast";
  }

  if (mkdir(PBS_DATA_DIR, 0777) && errno != EEXIST) {
    notify("PBS: cannot create /data/pbs");
    return 1;
  }

  if (!strcmp(mode, "avopen1") || !strcmp(mode, "avopenfast")) {
    /* Open the capture session; recovery retries only caller-owned resources. */
    if (argc < 3) return 1;
    const char *how = argc > 3 ? argv[3] : "none";
    int width = argc > 8 ? atoi(argv[8]) : 1280;
    int height = argc > 9 ? atoi(argv[9]) : 720;
    /* Diagnostic native frame-rate enum: 0=30, 1=60; 2 is unverified.
     * Streaming keeps the verified 60 fps profile and matching timestamps. */
    uint32_t fps_mode = argc > 10 ? (uint32_t)strtoul(argv[10], NULL, 0) : 1;
    if (fps_mode > 2 || (argc > 6 && !atoi(argv[6]) && fps_mode != 1)) return 1;
    if (!((width == 1280 && height == 720) || (width == 1920 && height == 1080) ||
          (width == 2560 && height == 1440) || (width == 3840 && height == 2160))) return 1;
    long (*call)(pbs_inject_t *, intptr_t, const uint64_t[6]) =
        !strcmp(mode, "avopenfast") ? pbs_inject_call_fast : pbs_inject_call;
avopen_retry:;
    if(live_request)live_start_attempts++;
    int clean_open_rejection = 0;
    FILE *f = open_report("inject.txt", "a");
    if (!f) return 1;
    fprintf(f, "%s %s %s\n", mode, argv[2], how);
    pbs_inject_t ctx;
    if (pbs_inject_begin(argv[2], &ctx)) {
      fprintf(f, "begin failed errno=%d\n", errno);
      fclose(f);
      return 1;
    }
    const intptr_t base = ctx.scratch;
    memset(scratch_zero, 0, sizeof scratch_zero);
    if (argc > 4) {
      /* RemotePlay parameter ABI: context+0x40 contains Params; Params+0x28 contains the feature ID. */
      uint64_t size = 0x2b0, common_size = 0x28;
      uint32_t feature = (uint32_t)strtoul(argv[4], NULL, 0);
      uint32_t usage = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 0) : 2;
      uint32_t resolution = width == 3840 ? 6 : width == 2560 ? 5 : width == 1920 ? 4 : 3;
      memcpy(scratch_zero + 8, &size, 8);
      memcpy(scratch_zero + 0x10, &common_size, 8);
      memcpy(scratch_zero + 8 + 0x14, &usage, 4);
      memcpy(scratch_zero + 8 + 0x18, &resolution, 4);
      memcpy(scratch_zero + 8 + 0x1c, &fps_mode, 4);
      memcpy(scratch_zero + 8 + 0x28, &feature, 4);
      /* ImagePaste uses the full output rectangle and all surface masks; offsets are relative to Params. */
      unsigned char *params = scratch_zero + 8;
      if (native_rate_config) {
        /* Native RemotePlay constructor +4dc4a..4dc6a: Params+90 is
         * a D0-byte encoder group, with a 20-byte rate descriptor at +98.
         * Daemon +17920 forwards nonzero rate/budget/initial/QP fields.
         * Keep initial capacity >= the backend's budget-per-frame floor. */
        uint32_t denominator = fps_mode == 0 ? 30000 : 60000;
        uint32_t initial = (uint32_t)(((uint64_t)requested_video_budget * 1001 + denominator - 1) / denominator);
        param_u64(params, 0x90, 0xd0);
        param_u64(params, 0x98, 0x20);
        param_u32(params, 0xa0, requested_video_bitrate);
        param_u32(params, 0xa4, requested_video_budget);
        param_u32(params, 0xa8, initial);
        param_u32(params, 0xac, requested_min_qp);
        param_u32(params, 0xb0, 51);
        fprintf(f, "Native rate config rate=%u companion=%u initial=%u QP=%u..51\n",
                requested_video_bitrate, requested_video_budget, initial, requested_min_qp);
      }
      param_u64(params, 0x1b0, 0x100);
      param_u64(params, 0x1b8, 0x40);
      param_u32(params, 0x1d0, width);
      param_u32(params, 0x1d4, height);
      param_u64(params, 0x1f8, 0x88);
      memset(params + 0x200, 0xff, 0x80);
      param_u32(params, 0x284, 2);
      param_u64(params, 0x288, 0x10);
      param_u32(params, 0x290, 1);
      params[0x294] = 1;
      param_u64(params, 0x298, 0x10);
      params[0x2a0] = 1;
      param_u32(params, 0x2a4, 0x6e);
      fprintf(f, "Params size=0x2b0 feature=%u usage=%u %dx%d fps-mode=%u\n", feature, usage, width, height, fps_mode);
    }
    intptr_t fn_init = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2Initialize");
    intptr_t fn_open = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2OpenVideo");
    intptr_t fn_oaudio = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2OpenAudio");
    intptr_t fn_term = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2Terminate");
    intptr_t fn_clear = 0;
    uint32_t grant_feature = 0;
    int granted_callback = 0;
    if (pbs_inject_write(&ctx, scratch_zero, 0x800)) {
      fprintf(f, "initial capture parameters could not be written\n");
      goto avopen_cleanup;
    }
    if (!strcmp(how, "tinit") && fn_term) {
      long tr = call(&ctx, fn_term, (uint64_t[6]){0, 0, 0, 0, 0, 0});
      fprintf(f, "Terminate() -> %s\n", fmt_rc(tr));
    }
    if (strcmp(how, "none") && fn_init) {
      long ir = call(&ctx, fn_init, (uint64_t[6]){0, 0, 0, 0, 0, 0});
      fprintf(f, "Initialize() -> %s\n", fmt_rc(ir));
    }
    if (!strcmp(how, "grant")) {
      grant_feature = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 0;
      /* Use capture profiles from ra-config.json. Do not modify features owned by active services. */
      if (strcmp(mode, "avopenfast") ||
          grant_feature != (width == 3840 ? 10u : width == 2560 ? 11u : 12u)) goto avopen_cleanup;
      intptr_t fn_set = pbs_resolve_in(ctx.pid, "libSceResourceArbitrator.sprx", "sceResourceArbitratorSetCallback");
      intptr_t fn_change = pbs_resolve_in(ctx.pid, "libSceResourceArbitrator.sprx", "sceResourceArbitratorChangeState");
      fn_clear = pbs_resolve_in(ctx.pid, "libSceResourceArbitrator.sprx", "sceResourceArbitratorClearCallback");
      /* Use getpid as a stable no-op callback that ignores notification arguments. */
      intptr_t callback = pbs_resolve_in(ctx.pid, "libkernel_sys.sprx", "getpid");
      if (!fn_set || !fn_change || !fn_clear || !callback) {
        fprintf(f, "missing resource arbitrator exports\n");
        goto avopen_cleanup;
      }
      long ar = call(&ctx, fn_set, (uint64_t[6]){grant_feature, (uint64_t)callback, 0, 0, 0, 0});
      fprintf(f, "SetCallback(%u) -> %s\n", grant_feature, fmt_rc(ar));
      if (ar == LONG_MIN || (int32_t)(uint32_t)ar < 0) goto avopen_cleanup;
      granted_callback = 1;
      char state[32] = "RUNNING";
      memcpy(scratch_zero + 0x600, state, sizeof state);
      if (pbs_inject_write(&ctx, scratch_zero, 0x800)) goto avopen_cleanup;
      ar = call(&ctx, fn_change, (uint64_t[6]){grant_feature, (uint64_t)base + 0x600, 0, 0, 0, 0});
      fprintf(f, "ChangeState(%u, RUNNING) -> %s\n", grant_feature, fmt_rc(ar));
      if (ar == LONG_MIN || (int32_t)(uint32_t)ar < 0) goto avopen_cleanup;
      intptr_t fn_enable = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2EnableScanInEncode");
      if (fn_enable) fprintf(f, "EnableScanInEncode() -> %s\n", fmt_rc(call(&ctx,
          fn_enable, (uint64_t[6]){0, 0, 0, 0, 0, 0})));
    }
    if (argc > 4) {
      intptr_t fn_mode = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2GetVideoOutMode");
      uint32_t video_mode = 0;
      long mr = fn_mode ? call(&ctx, fn_mode,
          (uint64_t[6]){(uint64_t)base + 0x700, 0, 0, 0, 0, 0}) : LONG_MIN;
      if (mr == LONG_MIN || (int32_t)(uint32_t)mr < 0 ||
          pbs_inject_read_at(&ctx, base + 0x700, &video_mode, sizeof video_mode)) {
        fprintf(f, "GetVideoOutMode failed -> %s\n", fmt_rc(mr));
        goto avopen_cleanup;
      }
      /* Params+0x280 must match the current video output mode, read immediately before OpenVideo. */
      memcpy(scratch_zero + 8 + 0x280, &video_mode, sizeof video_mode);
      if (pbs_inject_write(&ctx, scratch_zero, 0x800)) {
        fprintf(f, "video output mode could not be written\n");
        goto avopen_cleanup;
      }
      fprintf(f, "Params videoOutMode=0x%x\n", video_mode);
    }
    long rc = fn_open ? call(&ctx, fn_open,
                              (uint64_t[6]){(uint64_t)base, (uint64_t)base + 8, 0, 0, 0, 0}) : LONG_MIN;
    fprintf(f, "OpenVideo() -> %s%s\n", fmt_rc(rc), ctx.asserted ? " [ASSERT]" : "");
    if (fn_oaudio && strcmp(mode, "avopenfast")) {
      long ar = pbs_inject_call(&ctx, fn_oaudio,
                                (uint64_t[6]){(uint64_t)base + 0x10, (uint64_t)base + 0x18, 0, 0, 0, 0});
      fprintf(f, "OpenAudio() -> %s%s\n", fmt_rc(ar), ctx.asserted ? " [ASSERT]" : "");
    }
    unsigned char hb[16];
    if (!pbs_inject_read(&ctx, hb, sizeof hb)) {
      uint64_t h = 0;
      memcpy(&h, hb, 8);
      fprintf(f, "handle=0x%llx\n", (unsigned long long)h);
      clean_open_rejection = rc != LONG_MIN && (int32_t)(uint32_t)rc < 0 && !h && !ctx.asserted;
      if (!strcmp(mode, "avopenfast") && rc != LONG_MIN &&
          (int32_t)(uint32_t)rc >= 0 && h) {
        if (argc > 6) capture_stream(f, &ctx, h, atoi(argv[6]), argc > 7 ? atoi(argv[7]) : 8080, width, height, audio_mode);
        intptr_t close_fn = pbs_resolve_in(ctx.pid, "libSceAvcap2.sprx", "sceAvcap2Close");
        if (close_fn && !ctx.asserted) fprintf(f, "Close() -> %s\n", fmt_rc(call(&ctx, close_fn,
            (uint64_t[6]){h, 0, 0, 0, 0, 0})));
      }
    }
avopen_cleanup:;
    int clear_ok = !granted_callback;
    if (granted_callback && !ctx.asserted) {
      long clear_rc = call(&ctx, fn_clear, (uint64_t[6]){grant_feature, 0, 0, 0, 0, 0});
      fprintf(f, "ClearCallback(%u) -> %s\n", grant_feature, fmt_rc(clear_rc));
      clear_ok = clear_rc != LONG_MIN && (int32_t)(uint32_t)clear_rc >= 0;
    }
    pbs_inject_end(&ctx);
    fclose(f);
    int retry_start = !clean_open_rejection && clear_ok &&
        !live_server_started && live_start_attempts < 6;
    if (live_request && clear_ok && !ctx.asserted && (pbs_h264_server_running() || retry_start)) {
      usleep(250000);
      if (pbs_h264_server_running() || retry_start) goto avopen_retry;
    }
    if (live_request) { pbs_h264_server_stop(); pbs_h264_server_wait(); }
    notify(live_server_started ? "PBS: streaming stopped" : "PBS: capture unavailable");
    return live_server_started ? 0 : 1;
  }

  return 1;
}
