// SPDX-License-Identifier: GPL-3.0-or-later
/* Host-only mock Avcap source for exercising the *real* PS4 main capture
 * loop, HTTP transport and restart without access to a Sony service. */
#include "platform/ps4/capture/capture.h"
#include "platform/ps4/capture/request.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef PBS_TEST_WRAP_PUBLISH
int __real_pbs_h264_publish(const void *, size_t, uint32_t);
int __wrap_pbs_h264_publish(const void *data, size_t length, uint32_t pts) {
  static unsigned calls;
  if (++calls == 46) return -1;
  return __real_pbs_h264_publish(data, length, pts);
}
#endif

#define MAX_NALS 1024
typedef struct { unsigned char *data; size_t len; int key; } nal_t;
static nal_t nals[MAX_NALS];
static size_t nal_count, next_nal;
static unsigned char sps[128], pps[128];
static size_t sps_size, pps_size;
static FILE *mock_log;
static unsigned open_count, open_attempts, stop_count, close_count, term_count, frame_count;
static unsigned read_attempt_count, read_timeout_count;
static int mock_opened, mock_started;
static unsigned mock_getinfo_ready;
static uint64_t mock_open_at_ms;
static int mock_handle;

static uint64_t mock_milliseconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static int mock_delay_elapsed(const char *variable) {
  const char *value = getenv(variable);
  if (!mock_opened) return 0;
  if (!value) return 1;
  unsigned long delay = strtoul(value, NULL, 10);
  return mock_milliseconds() - mock_open_at_ms >= delay;
}

static size_t start_code(const unsigned char *p, size_t n, size_t offset) {
  if (offset + 2 >= n || p[offset] || p[offset+1]) return 0;
  if (p[offset+2] == 1) return 3;
  if (offset + 3 < n && !p[offset+2] && p[offset+3] == 1) return 4;
  return 0;
}

static int load_input(void) {
  const char *path = getenv("PBS_MOCK_H264");
  if (!path) return -1;
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END)) { fclose(f); return -1; }
  long size = ftell(f);
  if (size <= 0 || size > 16000000 || fseek(f, 0, SEEK_SET)) { fclose(f); return -1; }
  unsigned char *all = malloc((size_t)size);
  if (!all) { fclose(f); return -1; }
  int ok = fread(all, 1, (size_t)size, f) == (size_t)size;
  fclose(f);
  if (!ok) { free(all); return -1; }
  size_t cur = 0, n = (size_t)size;
  while (cur + 5 < n) {
    size_t prefix = start_code(all, n, cur);
    if (!prefix) { cur++; continue; }
    size_t next = cur + prefix;
    while (next < n && !start_code(all, n, next)) next++;
    if (cur + prefix < next) {
      unsigned type = all[cur + prefix] & 31;
      if (type == 7 && next - cur - prefix <= sizeof sps) {
        sps_size = next - cur - prefix;
        memcpy(sps, all + cur + prefix, sps_size);
      } else if (type == 8 && next - cur - prefix <= sizeof pps) {
        pps_size = next - cur - prefix;
        memcpy(pps, all + cur + prefix, pps_size);
      } else if ((type == 1 || type == 5) && nal_count < MAX_NALS) {
        nal_t *frame = &nals[nal_count++];
        frame->len = next - cur;
        frame->key = type == 5;
        frame->data = malloc(frame->len);
        if (!frame->data) { free(all); return -1; }
        memcpy(frame->data, all + cur, frame->len);
      }
    }
    cur = next;
  }
  free(all);
  return nal_count >= 45 && sps_size && pps_size ? 0 : -1;
}

int pbs_ps4_capture_begin(FILE *log) {
  mock_log = log;
  int rc = load_input();
  fprintf(log, "MOCK input NALs=%zu SPS=%zu PPS=%zu load=%d\n", nal_count, sps_size, pps_size, rc);
  return rc;
}
int pbs_ps4_init(void) { return 0; }
int pbs_ps4_static_info(unsigned type, void *out, size_t size) {
  if (size != 4) return -1;
  if (type == 24) {
    const char *slot_mask = getenv("PBS_MOCK_CAPTURE_SLOT_MASK");
    uint32_t mask = slot_mask ? (uint32_t)strtoul(slot_mask, NULL, 0) : 0;
    if (getenv("PBS_MOCK_CAPTURE_SLOT_DYNAMIC")) {
      /* A foreign owner rejects the first four OpenVideoEx attempts, then
       * the one successful local handle owns slot 0 until Close. */
      mask = mock_opened ? 1u : open_attempts <= 4 ? mask : 0u;
    }
    *(uint32_t *)out = mask;
    return 4; /* The native type 24 ABI returns the byte count. */
  }
  if (type != 21) return -1;
  *(uint32_t *)out = 3;
  return 0;
}
int pbs_ps4_remoteplay_status(unsigned char *status) {
  *status = getenv("PBS_MOCK_NATIVE_RP_ACTIVE") ? 1 : 0;
  return 0;
}
int pbs_ps4_output_mode_snapshot(unsigned long long *descriptor, unsigned char raw_output[0x28]) {
  *descriptor = mock_started ? 6 : mock_delay_elapsed("PBS_MOCK_MODE_EVENT_DELAY_MS") ? 5 : 4;
  if (raw_output) {
    memset(raw_output, 0, 0x28);
    memcpy(raw_output + 0x10, descriptor, 8);
  }
  return 0;
}
int pbs_ps4_videoout_bus_snapshot(unsigned *flags, unsigned char raw_output[0x28]) {
  *flags = mock_started || mock_delay_elapsed("PBS_MOCK_MODE_EVENT_DELAY_MS") ? 2 : 0;
  if (raw_output) {
    memset(raw_output, 0, 0x28);
    raw_output[6] = *flags ? 0x0d : 0x0c;
    memcpy(raw_output + 0x20, flags, sizeof *flags);
  }
  return 0;
}
int pbs_ps4_open(void **handle, const void *params) {
  const uint32_t *p = params;
  if (p[0x40/4] != 3749 || p[0x44/4] != 2657 || p[0x08/4] != 19 || p[1] != 0) return -1;
  ++open_attempts;
  const char *busy = getenv("PBS_MOCK_OPEN_BUSY_COUNT");
  if (busy && open_attempts <= strtoul(busy, NULL, 10)) {
    *handle = NULL;
    fprintf(mock_log, "MOCK OpenVideoEx transient 80a70008 attempt=%u without handle\n", open_attempts);
    return (int32_t)0x80a70008u;
  }
  open_count++;
  if (getenv("PBS_MOCK_FAIL_OPEN")) {
    fprintf(mock_log, "MOCK partial OpenVideoEx failure after local ID reservation\n");
    *handle = NULL;
    return -1;
  }
  mock_handle = 1;
  mock_opened = 1;
  mock_open_at_ms = mock_milliseconds();
  mock_getinfo_ready = 0;
  *handle = &mock_handle;
  next_nal = 0;
  fprintf(mock_log, "MOCK open count=%u\n", open_count);
  return 0;
}
int pbs_ps4_start(void *handle) {
  if (handle != &mock_handle) return -1;
  if (getenv("PBS_MOCK_FAIL_START")) {
    fprintf(mock_log, "MOCK Start failed after GetInfo\n");
    return -1;
  }
  if (getenv("PBS_MOCK_REQUIRE_PRESTART_INFO") && mock_getinfo_ready != 3) {
    fprintf(mock_log, "MOCK premature Start rejected: pre-Start GetInfo mask=%u\n", mock_getinfo_ready);
    return -1;
  }
  mock_started = 1;
  fprintf(mock_log, "MOCK Start accepted: pre-Start GetInfo mask=%u\n", mock_getinfo_ready);
  return 0;
}
int pbs_ps4_info(void *handle, unsigned type, void *out, size_t size) {
  if (handle != &mock_handle) return -1;
  if (getenv("PBS_MOCK_FAIL_INFO")) return -1;
  if (!mock_delay_elapsed("PBS_MOCK_INFO_DELAY_MS")) return -1;
  const unsigned char *src = type == 1 ? sps : pps;
  size_t count = type == 1 ? sps_size : pps_size;
  if ((type != 1 && type != 2) || count > size) return -1;
  memcpy(out, src, count);
  mock_getinfo_ready |= type == 1 ? 1u : 2u;
  return (int)count;
}
int pbs_ps4_read(void *handle, void *out, size_t size, void *info, int flags) {
  const char *expected = getenv("PBS_MOCK_EXPECT_READ_FLAGS");
  if (handle != &mock_handle || (flags != 0 && flags != 1) || !nal_count ||
      (expected && flags != expected[0] - '0')) return -1;
  /* The actual FW11 wrapper re-enters its 50ms wait after every successful
   * event wake even when the ring remains empty. Repeated positive signals
   * therefore NEVER return to the caller for flags=0. This fault reproduces
   * that path independently of normal timed-out waits. Only the already
   * documented flags=1 mode bypasses the wait and retains control. */
  if (getenv("PBS_MOCK_SPURIOUS_POSITIVE") && open_count == 1) {
    if (read_attempt_count++ == 0)
      fprintf(mock_log, "MOCK repeated positive event wakes with no frames flags=%d\n", flags);
    if (flags == 0) {
      unsigned signals = 0;
      for (;;) {
        usleep(2000); /* positive signal, empty ring, retry indefinitely */
        if (++signals == 64)
          fprintf(mock_log, "MOCK positive event wake count=64, zero frames, still inside ReadVideo\n");
      }
    }
    usleep(2000);
    return (int32_t)0x80a70002; /* nonblocking empty ring */
  }
  /* Other tests simulate the production flag explicitly, not the hidden
   * event-notification ABI. */
  /* A producer can remain entirely empty while the consumer is waiting.
   * Stress HTTP stop/restart during continuous native-sized wait timeouts.
   * The next Open may recover normally without a second native owner. */
  if (getenv("PBS_MOCK_STARVE_FIRST_OPEN") && open_count == 1) {
    /* The usual starvation fixture simulates native wait timeouts; a
     * distinct idle-CPU fixture returns an immediate empty-ring result
     * so the main loop's own 1ms throttle can be measured. */
    const int fast_empty = !!getenv("PBS_MOCK_FAST_EMPTY");
    if (!fast_empty) usleep(50000);
    ++read_timeout_count;
    if (read_timeout_count == 1) {
      if (fast_empty)
        fprintf(mock_log, "MOCK ReadVideo empty-ring nonblocking polling\n");
      else
        fprintf(mock_log, "MOCK ReadVideo starvation: first 50ms event timeout\n");
    }
    return (int32_t)0x80a70002;
  }
  /* Emulate the 0xc350-us event-flag wait in libSceAvcap: when the producer
   * has not filled its queue the native wrapper can time out once, without
   * invalidating a healthy handle or requiring Avcap restart. */
  if (getenv("PBS_MOCK_READ_WAIT_TIMEOUTS") && (++read_attempt_count % 7 == 0)) {
    usleep(50000);
    ++read_timeout_count;
    fprintf(mock_log, "MOCK ReadVideo event wait timeout count=%u\n", read_timeout_count);
    return (int32_t)0x80a70002;
  }
  const nal_t *frame = &nals[next_nal++ % nal_count];
  if (frame->len > size) return -1;
  memcpy(out, frame->data, frame->len);
  memset(info, 0, 0x40);
  frame_count++;
  usleep(16000);
  return (int)frame->len;
}
int pbs_ps4_keyframe(void *handle) {
  if (handle != &mock_handle) return -1;
  for (size_t n = 0; n < nal_count; ++n) if (nals[n].key) { next_nal = n; break; }
  return 0;
}
int pbs_ps4_stop(void *handle) {
  stop_count++;
  if (getenv("PBS_MOCK_FAIL_STOP")) return -1;
  mock_started = 0;
  return handle == &mock_handle ? 0 : -1;
}
int pbs_ps4_close(void *handle) {
  close_count++;
  if (getenv("PBS_MOCK_FAIL_CLOSE")) return -1;
  mock_opened = 0;
  return handle == &mock_handle ? 0 : -1;
}
int pbs_ps4_term(void) { term_count++; return 0; }
void pbs_ps4_capture_end(void) {
  fprintf(mock_log, "MOCK totals open=%u stop=%u close=%u term=%u frames=%u\n",
          open_count, stop_count, close_count, term_count, frame_count);
  if (getenv("PBS_MOCK_OPEN_BUSY_COUNT"))
    fprintf(mock_log, "MOCK OpenVideoEx total attempts=%u\n", open_attempts);
  if (getenv("PBS_MOCK_FAST_EMPTY"))
    fprintf(mock_log, "MOCK fast empty-ring reads=%u\n", read_timeout_count);
  for (size_t i = 0; i < nal_count; ++i) free(nals[i].data);
}

int pbs_ps4_entry(int argc, char **argv);
int main(int argc, char **argv) { return pbs_ps4_entry(argc, argv); }
