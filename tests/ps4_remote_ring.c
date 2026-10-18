// SPDX-License-Identifier: GPL-3.0-or-later
/* Reuse the remote process fixture, but exercise actual ring bytes instead
 * of mocking dequeue/consume results. No remote functions may run per read. */
#define main legacy_remote_main
#include "ps4_remote_capture.c"
#undef main
#include "ps4_avcap_ring.h"

#define RING_HANDLE 0x220000u
#define RING_MAPPING 0x240000u

static void set_ring(int32_t read, int32_t write, int32_t oldest,
                     uint32_t reader_gen, uint32_t writer_gen) {
  pbs_ps4_ring_cursor c = {RING_MAPPING, 0x1000, read, read, reader_gen};
  pbs_ps4_ring_header h = {0x1000, write, oldest, writer_gen};
  memcpy(target + RING_HANDLE + 8, &c, sizeof c);
  memcpy(target + RING_MAPPING, &h, sizeof h);
}

static void set_entry(int32_t offset, int32_t meta_at, int32_t payload_at,
                      int32_t reservation) {
  pbs_ps4_ring_entry e = {0x40, reservation, FRAME_LEN, 0};
  memcpy(target + RING_MAPPING + offset, &e, sizeof e);
  memset(target + RING_MAPPING + meta_at, 0x5a, 0x40);
  for (unsigned i = 0; i < FRAME_LEN; ++i)
    target[RING_MAPPING + payload_at + i] = (unsigned char)(i + 1);
}

int main(void) {
  FILE *log = tmpfile();
  assert(log);
  native_module_enabled = 0;
  assert(!pbs_ps4_remote_capture_begin(log));
  assert(!pbs_ps4_remote_capture_init());
  unsigned char status = 0xff;
  assert(!pbs_ps4_remote_status(&status) && status == 1);
  void *handle = (void *)(uintptr_t)RING_HANDLE;
  unsigned char frame[FRAME_LEN], info[0x40];
  set_ring(16, 196, 196, 0, 0);
  set_entry(16, 32, 96, FRAME_LEN);
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == FRAME_LEN);
  for (unsigned i = 0; i < FRAME_LEN; ++i) assert(frame[i] == (unsigned char)(i + 1));
  for (unsigned i = 0; i < sizeof info; ++i) assert(info[i] == 0x5a);
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == 0);
#if PBS_PS4_REMOTE_IDLE_RUN
  assert(idle_calls == 1);
  fail_idle = 1;
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) < 0);
  fail_idle = 0;
#else
  assert(idle_calls == 0);
#endif
  assert(dequeue_calls == 0 && consume_calls == 0);

  /* Metadata wraps independently of the payload reservation. */
  set_ring(0xfe0, 180, 180, 0, 1);
  set_entry(0xfe0, 16, 80, FRAME_LEN);
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == FRAME_LEN);
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == 0);

  /* Header wraps; reservation makes payload wrap although its actual length
   * would fit before the ring end. */
  set_ring(0xff8, 196, 196, 0, 1);
  set_entry(16, 32, 96, FRAME_LEN);
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == FRAME_LEN);
  set_ring(0xf00, 116, 116, 0, 1);
  set_entry(0xf00, 0xf10, 16, 0x100);
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == FRAME_LEN);

  /* Producer has lapped the consumer: resync once, signal discontinuity,
   * and expose no overwritten bytes as a successful frame. */
  set_ring(16, 196, 196, 0, 2);
  assert((uint32_t)pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == 0x80a70004u);
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == 0);

  /* The producer can wrap DURING PT_IO. Validate again after copying, not
   * merely before copying, or corrupted data would escape as a valid frame. */
  set_ring(16, 196, 196, 0, 0);
  set_entry(16, 32, 96, FRAME_LEN);
  mutate_after_read_address = RING_MAPPING + 96;
  mutate_header_address = RING_MAPPING;
  assert((uint32_t)pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == 0x80a70004u);
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == 0);

  /* Mapped bounds, producer errors and malformed entry lengths are errors. */
  set_ring(16, 196, 196, 0, 0);
  set_entry(16, 32, 96, FRAME_LEN);
  assert(pbs_ps4_remote_read(handle, frame, 10, info, 1) < 0);
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == 0);
  set_ring(16, 196, 196, 0, 0);
  pbs_ps4_ring_entry bad = {0x40, FRAME_LEN - 1, FRAME_LEN, 0};
  memcpy(target + RING_MAPPING + 16, &bad, sizeof bad);
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) < 0);
  int32_t error = (int32_t)0x80bf0011u;
  memcpy(target + RING_MAPPING, &error, sizeof error);
  assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 1) == error);
  assert(dequeue_calls == 0 && consume_calls == 0);
  assert(!pbs_ps4_remote_capture_end());
  fclose(log);
  puts("FW11 direct ring: copy, wraps, producer races, bounds, errors; no dequeue/consume RPC; idle-run checked when enabled");
  return 0;
}
