// SPDX-License-Identifier: GPL-3.0-or-later
/* Reuse the remote-service fault model, with invalid private helper/ring
 * offsets. The public route must never consult those fields. */
#define main legacy_main
#include "remote_capture.c"
#undef main

int main(void) {
  FILE *log = tmpfile();
  assert(log);
  native_module_enabled = 0;
  for (int session = 0; session < 2; ++session) {
    reset_counters();
    assert(!pbs_ps4_remote_capture_begin(log));
    assert(!pbs_ps4_remote_capture_init());
    uint32_t request[PBS_PS4_VIDEO_REQUEST_WORDS];
    assert(!pbs_ps4_make_video_request(request, 720, 0, 0));
    void *handle = NULL;
    assert(!pbs_ps4_remote_open(&handle, request) && handle);
    assert(open_calls == 3 && ready_calls == 0);
    assert(!pbs_ps4_remote_start(handle));
    unsigned char frame[FRAME_LEN], info[0x40];
    assert(pbs_ps4_remote_read(handle, frame, sizeof frame, info, 0) == FRAME_LEN);
    for (unsigned i = 0; i < FRAME_LEN; ++i) assert(frame[i] == (unsigned char)(i+1));
    for (unsigned i = 0; i < sizeof info; ++i) assert(info[i] == 0x5a);
    assert(!dequeue_calls && !consume_calls && exported_reads == session + 1);
    assert(!pbs_ps4_remote_stop(handle));
    assert(!pbs_ps4_remote_close(handle));
    assert(!pbs_ps4_remote_capture_end() && exported_frees == session + 1);
    assert(!pbs_ps4_remote_capture_active());
  }
  fclose(log);
  puts("Remote exported capture: invalid private offsets unused, Open retry, worker ReadVideo and repeated session release verified");
  return 0;
}
