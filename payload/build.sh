#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
platform=${1:-ps5}
kind=${2:-stream}
config=${PBS_BUILD_CONFIG:-Release}
case "$config" in
  Release) optimization=-O2; debug_flags=() ;;
  Debug) optimization=-O0; debug_flags=(-g) ;;
  *) echo 'PBS_BUILD_CONFIG must be Debug or Release' >&2; exit 2 ;;
esac
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
case "$platform" in
  ps4) sdk=${PS4_PAYLOAD_SDK:?Set PS4_PAYLOAD_SDK}; compiler="$sdk/bin/orbis-clang" ;;
  ps5) sdk=${PS5_PAYLOAD_SDK:?Set PS5_PAYLOAD_SDK}; compiler="$sdk/bin/prospero-clang" ;;
  *) echo 'Usage: build.sh ps4|ps5 [stream|probe]' >&2; exit 2 ;;
esac
out="$root/build/$platform"
mkdir -p "$out"
if [[ ${PBS_PS4_WORKER_PROBE:-0} == 1 && $kind == smoke && $platform == ps4 ]]; then
  kind=worker-probe
fi
if [[ $kind == worker-probe && $platform == ps4 ]]; then
  "$compiler" -O2 "${debug_flags[@]}" -Wall -Wextra -Wno-unused-parameter -DPBS_PS4 \
    "$root/src/ps4_worker_probe_main.c" "$root/src/ps4_worker_probe.c" \
    "$root/src/ps4_worker_boot.c" "$root/src/ps4_worker.c" \
    "$root/src/ps4_worker_remote.c" "$root/src/inject.c" \
    "$root/src/ps4_cred.c" "$root/src/ps4_control.c" \
    -o "$out/pbs-worker-probe.elf"
  python3 "$root/../tools/verify_ps4_worker_blob.py" "$out/pbs-worker-probe.elf"
elif [[ $kind == probe && $platform == ps4 ]]; then
  "$compiler" -O2 "${debug_flags[@]}" -Wall -Wextra -Werror "$root/src/ps4_probe.c" -o "$out/pbs-probe.elf"
elif [[ $kind == stream || ( $kind == smoke && $platform == ps4 ) ]]; then
  python3 "$root/embed_assets.py"
  if [[ $platform == ps4 ]]; then
    capture_height=${PBS_PS4_CAPTURE_HEIGHT:-720}
    if [[ $capture_height != 720 && $capture_height != 1080 ]]; then
      echo 'PBS_PS4_CAPTURE_HEIGHT must be 720 or 1080' >&2; exit 2
    fi
    capture_fps=${PBS_PS4_CAPTURE_FPS:-60}
    if [[ $capture_fps != 30 && $capture_fps != 60 ]]; then
      echo 'PBS_PS4_CAPTURE_FPS must be 30 or 60' >&2; exit 2
    fi
    if [[ $capture_fps == 30 && $capture_height != 1080 ]]; then
      echo 'The experimental 30fps route requires 1080p' >&2; exit 2
    fi
    capture_usage=${PBS_PS4_CAPTURE_USAGE:-0}
    image_paste=${PBS_PS4_IMAGE_PASTE:-0}
    read_wait=${PBS_PS4_READ_WAIT_EVENTS:-0}
    # The local libSceAvcap client is admitted as a different FW11 client
    # class and cannot open an encoder. Smoke defaults to the native-class
    # injected backend; production keeps the local backend until hardware
    # validation is complete. Set PBS_PS4_REMOTE_AVCAP=0/1 explicitly to
    # override either build.
    if [[ $kind == smoke && $capture_usage == 0 ]]; then
      remote_avcap=${PBS_PS4_REMOTE_AVCAP:-1}
    else
      remote_avcap=${PBS_PS4_REMOTE_AVCAP:-0}
    fi
    lifecycle=${PBS_PS4_REMOTE_LIFECYCLE:-0}
    idle_run=${PBS_PS4_REMOTE_IDLE_RUN:-0}
    if [[ $idle_run != 0 && $idle_run != 1 ]]; then
      echo 'PBS_PS4_REMOTE_IDLE_RUN must be 0 or 1' >&2; exit 2
    fi
    if [[ $idle_run == 1 && ( $kind != smoke || $remote_avcap != 1 ) ]]; then
      echo 'PBS_PS4_REMOTE_IDLE_RUN=1 requires a remote smoke build' >&2; exit 2
    fi
    # The capture runs on a dedicated worker thread inside SceRemotePlay so the
    # daemon is never frozen during the session. Default on for smoke builds,
    # off for production until hardware validation is complete.
    worker_boot=${PBS_PS4_WORKER_BOOT:--1}
    if [[ $worker_boot == -1 ]]; then
      worker_boot=0
      if [[ $remote_avcap == 1 ]]; then worker_boot=1; fi
    fi
    if [[ $worker_boot != 0 && $worker_boot != 1 ]]; then
      echo 'PBS_PS4_WORKER_BOOT must be 0 or 1' >&2; exit 2
    fi
    if [[ $worker_boot == 1 && $remote_avcap != 1 ]]; then
      echo 'PBS_PS4_WORKER_BOOT=1 requires PBS_PS4_REMOTE_AVCAP=1' >&2; exit 2
    fi
    if [[ $kind == stream && $remote_avcap == 1 && $worker_boot != 1 ]]; then
      echo 'Remote streaming requires the dedicated worker; use smoke for ptrace experiments' >&2; exit 2
    fi
    if [[ $capture_usage != 0 && $capture_usage != 1 ]]; then
      echo 'PBS_PS4_CAPTURE_USAGE must be 0 or 1' >&2; exit 2
    fi
    if [[ $image_paste != 0 && $image_paste != 1 ]]; then
      echo 'PBS_PS4_IMAGE_PASTE must be 0 or 1' >&2; exit 2
    fi
    if [[ $read_wait != 0 && $read_wait != 1 ]]; then
      echo 'PBS_PS4_READ_WAIT_EVENTS must be 0 or 1' >&2; exit 2
    fi
    if [[ $remote_avcap != 0 && $remote_avcap != 1 ]]; then
      echo 'PBS_PS4_REMOTE_AVCAP must be 0 or 1' >&2; exit 2
    fi
    if [[ $lifecycle != 0 && $lifecycle != 1 ]]; then
      echo 'PBS_PS4_REMOTE_LIFECYCLE must be 0 or 1' >&2; exit 2
    fi
    if [[ $lifecycle == 1 && $remote_avcap == 0 ]]; then
      echo 'PBS_PS4_REMOTE_LIFECYCLE=1 requires PBS_PS4_REMOTE_AVCAP=1' >&2; exit 2
    fi
    defines=(-DPBS_PS4 "-DPBS_PS4_CAPTURE_USAGE=$capture_usage" "-DPBS_PS4_IMAGE_PASTE=$image_paste" "-DPBS_PS4_READ_WAIT_EVENTS=$read_wait" "-DPBS_PS4_REMOTE_AVCAP=$remote_avcap" "-DPBS_PS4_REMOTE_LIFECYCLE=$lifecycle")
    if [[ $capture_fps == 30 && ( $capture_usage != 0 || $image_paste != 0 || $remote_avcap != 1 ) ]]; then
      echo 'The generic 1080p30 experiment requires remote usage=0 without ImagePaste' >&2; exit 2
    fi
    remote_1080=${PBS_PS4_REMOTE_1080:-0}
    if [[ $remote_1080 != 0 && $remote_1080 != 1 ]]; then
      echo 'PBS_PS4_REMOTE_1080 must be 0 or 1' >&2; exit 2
    fi
    remote_request=3
    if [[ $capture_fps == 30 && $remote_1080 == 0 ]]; then remote_request=0; fi
    admission_probe=${PBS_PS4_ADMISSION_PROBE:-0}
    latch_bypass=${PBS_PS4_LATCH_BYPASS:-0}
    if [[ $latch_bypass != 0 && $latch_bypass != 1 ]]; then
      echo 'PBS_PS4_LATCH_BYPASS must be 0 or 1' >&2; exit 2
    fi
    budget_unlock=${PBS_PS4_BUDGET_UNLOCK:-0}
    if [[ $budget_unlock != 0 && $budget_unlock != 1 ]]; then
      echo 'PBS_PS4_BUDGET_UNLOCK must be 0 or 1' >&2; exit 2
    fi
    low_memory=${PBS_PS4_LOW_MEMORY:-0}
    if [[ $low_memory != 0 && $low_memory != 1 ]]; then
      echo 'PBS_PS4_LOW_MEMORY must be 0 or 1' >&2; exit 2
    fi
    if [[ $remote_1080 == 1 ]]; then
      low_memory=1
      if [[ $kind != smoke || $capture_height != 1080 || $capture_fps != 30 || $remote_avcap != 1 || $worker_boot != 1 || $capture_usage != 0 || $image_paste != 0 || $latch_bypass != 0 || $budget_unlock != 0 || $admission_probe != 0 || $lifecycle != 0 || $idle_run != 0 ]]; then
        echo 'Remote1080 requires a normal worker 1080p30 smoke without latch/budget/admission/lifecycle/idle flags' >&2; exit 2
      fi
    elif [[ $low_memory == 1 && ( $kind != smoke || $capture_height != 1080 || $capture_fps != 30 || $latch_bypass != 1 || $budget_unlock != 0 ) ]]; then
      echo 'Low-memory 1080p30 requires a latch smoke with the Neo budget flag disabled' >&2; exit 2
    fi
    venc_journal=${PBS_PS4_VENC_JOURNAL:-0}
    if [[ $venc_journal != 0 && $venc_journal != 1 ]]; then
      echo 'PBS_PS4_VENC_JOURNAL must be 0 or 1' >&2; exit 2
    fi
    if [[ $venc_journal == 1 && $remote_1080 != 1 ]]; then
      echo 'Venc printf journal requires the bounded Remote1080 experiment' >&2; exit 2
    fi
    native_source=${PBS_PS4_NATIVE_SOURCE:-0}
    if [[ $native_source != 0 && $native_source != 1 ]]; then
      echo 'PBS_PS4_NATIVE_SOURCE must be 0 or 1' >&2; exit 2
    fi
    if [[ $native_source == 1 && ( $remote_1080 != 1 || $venc_journal != 0 ) ]]; then
      echo 'Native source requires Remote1080 without journal' >&2; exit 2
    fi
    defines+=("-DPBS_PS4_NATIVE_SOURCE=$native_source")
    memory_count=${PBS_PS4_MEMORY_COUNT:-3}
    if [[ $memory_count != 3 && $memory_count != 4 && $memory_count != 5 ]]; then
      echo 'PBS_PS4_MEMORY_COUNT must be 3, 4 or 5' >&2; exit 2
    fi
    if [[ $memory_count != 3 && $low_memory != 1 ]]; then
      echo 'Nondefault memory count requires the bounded low-memory experiment' >&2; exit 2
    fi
    daemon_dump=${PBS_PS4_DAEMON_DUMP:-0}
    if [[ $daemon_dump != 0 && $daemon_dump != 1 ]]; then
      echo 'PBS_PS4_DAEMON_DUMP must be 0 or 1' >&2; exit 2
    fi
    encoder_state=${PBS_PS4_ENCODER_STATE:-0}
    if [[ $encoder_state != 0 && $encoder_state != 1 ]]; then
      echo 'PBS_PS4_ENCODER_STATE must be 0 or 1' >&2; exit 2
    fi
    if [[ $encoder_state == 1 ]]; then
      if [[ $kind != smoke || $capture_height != 720 || $capture_fps != 60 || $remote_avcap != 1 || $worker_boot != 1 || $capture_usage != 0 || $image_paste != 0 || $lifecycle != 0 || $idle_run != 0 || $low_memory != 0 || $admission_probe != 0 || $latch_bypass != 0 || $budget_unlock != 0 || $venc_journal != 0 || $native_source != 0 || $daemon_dump != 0 ]]; then
        echo 'PBS_PS4_ENCODER_STATE=1 requires the standard working 720p60 worker smoke without other experiments' >&2; exit 2
      fi
    fi
    defines+=("-DPBS_PS4_ENCODER_STATE=$encoder_state")
    if [[ $latch_bypass == 1 ]]; then admission_probe=1; fi
    if [[ $budget_unlock == 1 ]]; then admission_probe=1; fi
    if [[ $admission_probe != 0 && $admission_probe != 1 ]]; then
      echo 'PBS_PS4_ADMISSION_PROBE must be 0 or 1' >&2; exit 2
    fi
    if [[ $admission_probe == 1 && ( $kind != smoke || $capture_height != 1080 || $capture_fps != 30 || $remote_avcap != 1 || $worker_boot != 1 || $lifecycle != 0 || $idle_run != 0 ) ]]; then
      echo 'Admission diagnostics require a worker 1080p30 smoke without lifecycle/idle experiments' >&2; exit 2
    fi
    defines+=("-DPBS_PS4_ADMISSION_PROBE=$admission_probe")
    defines+=("-DPBS_PS4_LATCH_BYPASS=$latch_bypass")
    defines+=("-DPBS_PS4_BUDGET_UNLOCK=$budget_unlock")
    defines+=("-DPBS_PS4_LOW_MEMORY=$low_memory" "-DPBS_PS4_REMOTE_1080=$remote_1080" "-DPBS_PS4_MEMORY_COUNT=$memory_count")
    defines+=("-DPBS_PS4_DAEMON_DUMP=$daemon_dump" "-DPBS_PS4_VENC_JOURNAL=$venc_journal")
    defines+=("-DPBS_PS4_CAPTURE_HEIGHT=$capture_height" "-DPBS_PS4_CAPTURE_FPS=$capture_fps" "-DPBS_PS4_REMOTE_REQUEST=$remote_request")
    defines+=("-DPBS_PS4_REMOTE_IDLE_RUN=$idle_run")
    defines+=("-DPBS_PS4_WORKER_BOOT=$worker_boot")
    sources=("$root/src/ps4_main.c" "$root/src/ps4_capture.c")
    if [[ $remote_avcap == 1 ]]; then
      sources+=("$root/src/ps4_remote_capture.c" "$root/src/ps4_worker.c" "$root/src/ps4_worker_remote.c" "$root/src/ps4_worker_boot.c")
    fi
    sources+=("$root/src/ps4_control.c" "$root/src/inject.c" "$root/src/h264_http.c" "$root/src/ps4_cred.c")
    if [[ $admission_probe == 1 || $low_memory == 1 || $encoder_state == 1 ]]; then
      sources+=("$root/src/ps4_snapshot.c" "$root/src/ps4_admission.c")
    fi
    if [[ $latch_bypass == 1 || $budget_unlock == 1 || $native_source == 1 ]]; then sources+=("$root/src/ps4_latch_patch.c"); fi
    if [[ $venc_journal == 1 ]]; then sources+=("$root/src/ps4_venc_journal.c"); fi
    if [[ $daemon_dump == 1 ]]; then sources+=("$root/src/ps4_daemon_dump.c"); fi
  else
    sources=("$root/src/main.c" "$root/src/inject.c" "$root/src/h264_http.c")
    defines=()
  fi
  filename=pbs.elf
  if [[ $platform == ps4 && $kind == stream && $remote_avcap == 1 ]]; then
    filename=pbs-remote-stream.elf
  fi
  if [[ $kind == smoke ]]; then
    capture_seconds=${PBS_PS4_CAPTURE_SECONDS:-10}
    if [[ ! $capture_seconds =~ ^[1-9][0-9]*$ ]] || (( capture_seconds > 3600 )); then
      echo 'PBS_PS4_CAPTURE_SECONDS must be 1..3600' >&2; exit 2
    fi
    if [[ $platform == ps4 && $low_memory == 1 ]] && (( capture_seconds > 10 )); then
      echo 'Low-memory 1080p experiment is limited to 10 seconds' >&2; exit 2
    fi
    defines+=("-DPBS_PS4_CAPTURE_SECONDS=$capture_seconds" -DPBS_PS4_OBSERVE_REMOTEPLAY_STATUS=1)
    observers=${PBS_PS4_SMOKE_OBSERVERS:-0}
    if [[ $observers != 0 && $observers != 1 ]]; then
      echo 'PBS_PS4_SMOKE_OBSERVERS must be 0 or 1' >&2; exit 2
    fi
    # FW11 hardware smoke crashed in the optional AvSetting/VideoOut
    # observation phase before Init/Open. Keep its ABI-dependent dlopen and
    # GetCurrentOutputMode2_ out of the normal capture path; preserve this
    # instrumentation only in a separately named diagnostic ELF.
    if [[ $observers == 1 ]]; then
      defines+=(-DPBS_PS4_OBSERVE_VIDEO_MODE=1 -DPBS_PS4_OBSERVE_VIDEOOUT_BUS=1)
    fi
    source_wait=${PBS_PS4_SMOKE_WAIT_SOURCE:-0}
    if [[ $source_wait != 0 && $source_wait != 1 ]]; then
      echo 'PBS_PS4_SMOKE_WAIT_SOURCE must be 0 or 1' >&2; exit 2
    fi
    if [[ $source_wait == 1 ]]; then
      if [[ $observers != 1 ]]; then
        echo 'PBS_PS4_SMOKE_WAIT_SOURCE=1 requires PBS_PS4_SMOKE_OBSERVERS=1' >&2; exit 2
      fi
      defines+=(-DPBS_PS4_SMOKE_WAIT_SOURCE=1)
    fi
    filename=pbs-smoke.elf
    if [[ $capture_seconds != 10 ]]; then filename=pbs-smoke-${capture_seconds}s.elf; fi
  fi
  if [[ $platform == ps4 && $capture_height == 1080 ]]; then
    filename=${filename%.elf}-1080p.elf
  fi
  if [[ $platform == ps4 && $capture_fps == 30 ]]; then
    filename=${filename%.elf}-30fps.elf
  fi
  if [[ $platform == ps4 && $capture_usage == 1 ]]; then
    filename=${filename%.elf}-usage1.elf
  fi
  if [[ $platform == ps4 && $image_paste == 1 ]]; then
    filename=${filename%.elf}-imagepaste.elf
  fi
  if [[ $platform == ps4 && $kind == smoke && $observers == 1 ]]; then
    filename=${filename%.elf}-observers.elf
  fi
  if [[ $platform == ps4 && $read_wait == 1 ]]; then
    filename=${filename%.elf}-nativewait.elf
  fi
  if [[ $platform == ps4 && $kind == smoke && $capture_usage == 0 && $remote_avcap == 0 ]]; then
    filename=${filename%.elf}-local.elf
  fi
  if [[ $platform == ps4 && $kind == smoke && $lifecycle == 1 ]]; then
    filename=${filename%.elf}-lifecycle.elf
  fi
  if [[ $platform == ps4 && $idle_run == 1 ]]; then
    filename=${filename%.elf}-idle-run.elf
  fi
  if [[ $platform == ps4 && $admission_probe == 1 ]]; then
    filename=${filename%.elf}-admission.elf
  fi
  if [[ $platform == ps4 && $latch_bypass == 1 ]]; then
    filename=${filename%.elf}-latch.elf
  fi
  if [[ $platform == ps4 && $budget_unlock == 1 ]]; then
    filename=${filename%.elf}-budget.elf
  fi
  if [[ $platform == ps4 && $daemon_dump == 1 ]]; then
    filename=${filename%.elf}-daemondump.elf
  fi
  if [[ $platform == ps4 && $encoder_state == 1 ]]; then
    filename=${filename%.elf}-encoderstate.elf
  fi
  if [[ $platform == ps4 && $low_memory == 1 ]]; then
    filename=${filename%.elf}-lowmem.elf
  fi
  if [[ $platform == ps4 && $remote_1080 == 1 ]]; then
    filename=${filename%.elf}-rp24.elf
  fi
  if [[ $platform == ps4 && $memory_count != 3 ]]; then
    filename=${filename%.elf}-memory${memory_count}.elf
  fi
  if [[ $platform == ps4 && $venc_journal == 1 ]]; then
    filename=${filename%.elf}-vencjournal.elf
  fi
  if [[ $platform == ps4 && $native_source == 1 ]]; then
    filename=${filename%.elf}-nativesource.elf
  fi
  "$compiler" "$optimization" "${debug_flags[@]}" -Wall -Wextra -Wno-unused-parameter "${defines[@]}" "${sources[@]}" -o "$out/$filename"
  if [[ $platform == ps4 && $remote_avcap == 1 ]]; then
    python3 "$root/../tools/verify_ps4_worker_blob.py" "$out/$filename"
  fi
else
  echo 'Unsupported platform/build combination' >&2; exit 2
fi
