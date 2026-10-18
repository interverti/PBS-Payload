#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Offline regression only. Never connects to a console or its ELF loader.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
cc=${CC:-cc}
out=build/ps4-tests
mkdir -p "$out"
flags=(-std=gnu11 -O2 -Wall -Wextra -Werror -Ipayload/src -Itests/stubs)
run() {
  local name=$1
  shift
  "$cc" "${flags[@]}" "$@" -o "$out/$name"
  "$out/$name"
}
run firmware tests/ps4_firmware.c
run snapshot -DPBS_PS4 tests/ps4_snapshot.c payload/src/ps4_snapshot.c
run admission -DPBS_PS4 -DPBS_TEST_CONTROL tests/ps4_admission.c payload/src/ps4_admission.c
run encoder-snapshot -DPBS_PS4 -DPBS_TEST_CONTROL tests/ps4_encoder_snapshot.c payload/src/ps4_admission.c
run venc-journal -DPBS_PS4 -DPBS_TEST_CONTROL tests/ps4_venc_journal.c payload/src/ps4_venc_journal.c
run latch-patch -DPBS_PS4 -DPBS_TEST_CONTROL -DPBS_PS4_BUDGET_UNLOCK=1 tests/ps4_latch_patch.c payload/src/ps4_latch_patch.c
run source-cap -DPBS_PS4 -DPBS_TEST_CONTROL -DPBS_PS4_BUDGET_UNLOCK=0 -DPBS_PS4_NATIVE_SOURCE=1 tests/ps4_latch_patch.c payload/src/ps4_latch_patch.c
run request-native-source -DPBS_PS4_NATIVE_SOURCE=1 tests/ps4_video_request.c
run snapshot-native-source -DPBS_PS4 -DPBS_TEST_CONTROL -DPBS_PS4_NATIVE_SOURCE=1 tests/ps4_encoder_snapshot.c payload/src/ps4_admission.c
run remote-native-source -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_RING_COPY=0 -DPBS_PS4_REMOTE_1080=1 -DPBS_PS4_NATIVE_SOURCE=1 -DPBS_PS4_LOW_MEMORY=1 tests/ps4_latch_remote.c payload/src/ps4_remote_capture.c
run latch-patch-only -DPBS_PS4 -DPBS_TEST_CONTROL -DPBS_PS4_BUDGET_UNLOCK=0 tests/ps4_latch_patch.c payload/src/ps4_latch_patch.c
run daemon-dump -DPBS_PS4 -DPBS_TEST_CONTROL tests/ps4_daemon_dump.c payload/src/ps4_daemon_dump.c
run trampoline -mavx -no-pie tests/ps4_call_trampoline.c
run request tests/ps4_video_request.c
run request-memory5 -DPBS_PS4_MEMORY_COUNT=5 tests/ps4_video_request.c
run privacy tests/ps4_video_privacy.c
run worker-socket -pthread tests/ps4_worker_socket.c payload/src/ps4_worker.c payload/src/ps4_worker_remote.c
run worker-boot -DPBS_PS4 tests/ps4_worker_boot.c payload/src/ps4_worker_boot.c payload/src/ps4_worker_remote.c
run worker-probe -DPBS_PS4 tests/ps4_worker_probe.c payload/src/ps4_worker_probe.c
for worker in 0 1; do
  run control-$worker -DPBS_TEST_CONTROL -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=$worker tests/ps4_control_rollback.c payload/src/ps4_control.c
  for scenario in init-timeout set-timeout set-negative replaced-process identity-unavailable gate-mismatch; do
    "$out/control-$worker" "$scenario"
  done
done
"$cc" "${flags[@]}" -DPBS_TEST_CONTROL -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_REQUEST=0 tests/ps4_control_rollback.c payload/src/ps4_control.c -o "$out/control-generic"
"$out/control-generic" generic-profile
"$cc" "${flags[@]}" -DPBS_TEST_CONTROL -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_TEST_FW_VERSION=0x12008001u tests/ps4_control_rollback.c payload/src/ps4_control.c -o "$out/unsupported"
"$out/unsupported" unsupported-firmware
for worker in 0 1; do
  run remote-$worker -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=$worker -DPBS_PS4_REMOTE_RING_COPY=0 tests/ps4_remote_capture.c payload/src/ps4_remote_capture.c
done
run remote-rp24 -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_RING_COPY=0 -DPBS_PS4_REMOTE_1080=1 tests/ps4_remote_capture.c payload/src/ps4_remote_capture.c
run remote-admission -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_RING_COPY=0 -DPBS_PS4_ADMISSION_PROBE=1 tests/ps4_remote_capture.c payload/src/ps4_remote_capture.c
run remote-latch -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_RING_COPY=0 -DPBS_PS4_ADMISSION_PROBE=1 -DPBS_PS4_LATCH_BYPASS=1 tests/ps4_latch_remote.c payload/src/ps4_remote_capture.c
run remote-lowmem -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_RING_COPY=0 -DPBS_PS4_ADMISSION_PROBE=1 -DPBS_PS4_LATCH_BYPASS=1 -DPBS_PS4_LOW_MEMORY=1 tests/ps4_latch_remote.c payload/src/ps4_remote_capture.c
run remote-budget -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_RING_COPY=0 -DPBS_PS4_ADMISSION_PROBE=1 -DPBS_PS4_LATCH_BYPASS=1 -DPBS_PS4_BUDGET_UNLOCK=1 tests/ps4_latch_remote.c payload/src/ps4_remote_capture.c
run remote-budget-only -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 -DPBS_PS4_REMOTE_RING_COPY=0 -DPBS_PS4_ADMISSION_PROBE=1 -DPBS_PS4_BUDGET_UNLOCK=1 tests/ps4_budget_remote.c payload/src/ps4_remote_capture.c
run ring -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 tests/ps4_remote_ring.c payload/src/ps4_remote_capture.c
run ring-worker -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_WORKER_BOOT=1 tests/ps4_remote_ring.c payload/src/ps4_remote_capture.c
run ownership tests/ps4_capture_ownership.c payload/src/ps4_capture.c
for scenario in local-term-fail local-init-negative native-activated; do
  "$out/ownership" "$scenario"
done
run lifecycle -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_REMOTE_LIFECYCLE=1 -DPBS_PS4_REMOTE_RING_COPY=0 tests/ps4_remote_lifecycle.c payload/src/ps4_remote_capture.c
run lifecycle-release -DPBS_PS4 -DPBS_PS4_REMOTE_AVCAP=1 -DPBS_PS4_REMOTE_LIFECYCLE=1 -DPBS_PS4_REMOTE_RING_COPY=0 tests/ps4_lifecycle_release.c payload/src/ps4_capture.c payload/src/ps4_remote_capture.c
python3 payload/embed_assets.py
run http-shutdown -pthread tests/http_shutdown.c payload/src/h264_http.c
run http-frame-rate -pthread tests/http_frame_rate.c payload/src/h264_http.c
if command -v node >/dev/null 2>&1; then
  node tests/h264_mux_rate.js
else
  printf 'SKIP browser frame-rate regression: Node.js unavailable.\n'
fi
record_dir=$(mktemp -d "$PWD/$out/recording.XXXXXX")
"$cc" "${flags[@]}" -pthread "-DPBS_RECORD_DIR=\"$record_dir\"" tests/http_record_restart.c payload/src/h264_http.c -o "$out/http-record"
"$out/http-record" "$record_dir"
python3 -m unittest discover -s tests -p test_verify_ps4_capture.py
python3 -m unittest discover -s tests -p test_inspect_ps4_venc_journal.py
printf 'PS4 offline regression passed. No console connection made.\n'
