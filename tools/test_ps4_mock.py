#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the PS4 capture main loop with an encoded *visible* test source.

Run under WSL/Linux after making an Annex B H.264 fixture with FFmpeg. Uses
real PBS HTTP and restart logic, while simulating only the missing Sony Avcap
source. This does not establish that the console supplies actual pixels.
"""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "ps4-mock"


def build(cc, probe_empty=False, fail_publish=False, source_barrier=False, native_wait=False,
          observe_open_slots=False):
    OUT.mkdir(parents=True, exist_ok=True)
    source = ROOT / "payload" / "src"
    flags = [cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-pthread",
             "-DPBS_PS4_OBSERVE_REMOTEPLAY_STATUS=1", "-DPBS_PS4_OBSERVE_VIDEO_MODE=1",
             "-DPBS_PS4_OBSERVE_VIDEOOUT_BUS=1",
             "-I" + str(ROOT / "tests" / "stubs"), "-I" + str(source),
             '-DPBS_PS4_DATA_ROOT="build/ps4-mock"']
    if probe_empty:
        flags.append("-DPBS_PS4_SMOKE_REOPEN_EMPTY=1")
    if fail_publish:
        flags.append("-DPBS_TEST_WRAP_PUBLISH=1")
    if source_barrier:
        flags.append("-DPBS_PS4_SMOKE_WAIT_SOURCE=1")
    if native_wait:
        flags.append("-DPBS_PS4_READ_WAIT_EVENTS=1")
    if observe_open_slots:
        flags.append("-DPBS_PS4_TEST_OPEN_SLOTS=1")
    subprocess.run(flags + ["-Dmain=pbs_ps4_entry", "-c", str(source / "ps4_main.c"),
                            "-o", str(OUT / "entry.o")], cwd=ROOT, check=True)
    linking = ["-Wl,--wrap=pbs_h264_publish"] if fail_publish else []
    subprocess.run(flags + linking + [str(OUT / "entry.o"),
                            str(ROOT / "tests" / "ps4_mock_avcap.c"),
                            str(source / "h264_http.c"),
                            "-o", str(OUT / "mock-server")], cwd=ROOT, check=True)


def wait_config(url, process):
    for _ in range(100):
        if process.poll() is not None:
            raise RuntimeError("Mock payload terminated before HTTP startup")
        try:
            with urllib.request.urlopen(url + "/config.json", timeout=0.2) as response:
                return json.load(response)
        except (OSError, urllib.error.URLError, TimeoutError):
            time.sleep(0.04)
    raise RuntimeError("Mock payload did not open HTTP server")


def pull_frames(url, target, count, wait):
    with urllib.request.urlopen(url + "/frames", timeout=3) as response:
        for _ in range(count):
            header = response.read(8)
            if len(header) != 8:
                raise RuntimeError("HTTP stream disconnected before expected frames")
            size, pts = struct.unpack(">II", header)
            if size < 5 or size > 2**21:
                raise RuntimeError(f"Invalid PBS media frame size {size}")
            frame = response.read(size)
            if len(frame) != size:
                raise RuntimeError("Partial video access unit")
            target.write(frame)
            wait.append(pts)
    if any(b <= a for a, b in zip(wait, wait[1:])):
        raise RuntimeError("Video timestamps must be monotonic")


def linux_process_cpu_ms(pid):
    """CPU consumed by a host-mock process, not a PS4 hardware estimate."""
    fields = Path(f"/proc/{pid}/stat").read_text(encoding="ascii").rsplit(")", 1)[1].split()
    # /proc stat: utime=field14, stime=field15; this tail starts at field3.
    return 1000 * (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sample", type=Path, help="Annex B H.264 with >=45 visible frames")
    parser.add_argument("--cc", default="cc", help="Native Linux C compiler")
    parser.add_argument("--port", type=int, default=18183)
    parser.add_argument("--expect-low-bitrate", action="store_true",
                        help="Assert the capture loop flags a known near-empty black test source")
    parser.add_argument("--inject-error", choices=("none", "open", "start", "info", "stop", "close", "publish"), default="none",
                        help="Force Stop/Close failure after restart; ensure PS4 aborts reopen")
    parser.add_argument("--probe-empty", action="store_true",
                        help="Test smoke-only automatic bounded reopen for tiny black H.264 input")
    parser.add_argument("--delayed-headers-ms", type=int, default=0,
                        help="Simulate delayed native Avcap GetInfo before Start")
    parser.add_argument("--source-barrier", action="store_true",
                        help="Enable bounded, read-only output-state settling before Start")
    parser.add_argument("--delayed-mode-ms", type=int, default=0,
                        help="Simulate delayed asynchronous output-mode transition after Open")
    parser.add_argument("--never-mode-event", action="store_true",
                        help="Simulate a missing output-mode event and require a bounded, unverified timeout")
    parser.add_argument("--read-event-timeouts", action="store_true",
                        help="Inject native ReadVideo event waits/timeouts without invalidating the session")
    parser.add_argument("--starved-control", choices=("stop", "restart"),
                        help="Exercise an HTTP control request while ReadVideo only returns 50ms event timeouts")
    parser.add_argument("--spurious-positive-control", choices=("stop", "restart"),
                        help="Positive producer wakeups with empty ring: control must remain serviceable")
    parser.add_argument("--native-read-wait", action="store_true",
                        help="Unsafe regression control: compile native flags=0 and demonstrate uncancellable reads")
    parser.add_argument("--spurious-partial-client", action="store_true",
                        help="Also keep an incomplete HTTP client connected while stopping the empty-ring reader")
    parser.add_argument("--restart-burst-during-open", action="store_true",
                        help="Send a duplicate restart while the next Avcap Open/GetInfo is still initializing")
    parser.add_argument("--idle-cpu", action="store_true",
                        help="Measure CPU when the nonblocking reader has no packets and auto-recovers")
    parser.add_argument("--open-busy", choices=("recover", "exhaust"),
                        help="FW11 OpenVideoEx 80a70008: native bounded retry vs fatal exhausted budget")
    args = parser.parse_args()
    sample = args.sample.resolve()
    if not sample.is_file():
        parser.error(f"Missing H.264 input fixture: {sample}")
    if args.probe_empty and (not args.expect_low_bitrate or args.inject_error != "none"):
        parser.error("--probe-empty needs --expect-low-bitrate and no injected Stop/Close failure")
    if args.delayed_headers_ms < 0 or args.delayed_headers_ms > 1200 or args.delayed_mode_ms < 0 or args.delayed_mode_ms > 1200:
        parser.error("Delayed fixture values must be between 0 and 1200ms")
    if args.never_mode_event and (not args.source_barrier or args.delayed_mode_ms):
        parser.error("--never-mode-event requires --source-barrier and no --delayed-mode-ms")
    if args.spurious_partial_client and args.spurious_positive_control != "stop":
        parser.error("--spurious-partial-client requires --spurious-positive-control stop")
    build(args.cc, args.probe_empty, args.inject_error == "publish", args.source_barrier,
          args.native_read_wait, bool(args.open_busy))
    output = OUT / "client.h264"
    env = os.environ.copy()
    env["PBS_MOCK_H264"] = str(sample)
    env["PBS_MOCK_REQUIRE_PRESTART_INFO"] = "1"
    env["PBS_MOCK_EXPECT_READ_FLAGS"] = "0" if args.native_read_wait else "1"
    if args.delayed_headers_ms:
        env["PBS_MOCK_INFO_DELAY_MS"] = str(args.delayed_headers_ms)
    if args.delayed_mode_ms:
        env["PBS_MOCK_MODE_EVENT_DELAY_MS"] = str(args.delayed_mode_ms)
    if args.never_mode_event:
        env["PBS_MOCK_MODE_EVENT_DELAY_MS"] = "100000"
    if args.read_event_timeouts:
        env["PBS_MOCK_READ_WAIT_TIMEOUTS"] = "1"
    if args.spurious_positive_control:
        env["PBS_MOCK_SPURIOUS_POSITIVE"] = "1"
    if args.starved_control or args.restart_burst_during_open or args.idle_cpu:
        env["PBS_MOCK_STARVE_FIRST_OPEN"] = "1"
    if args.idle_cpu:
        env["PBS_MOCK_FAST_EMPTY"] = "1"
    if args.restart_burst_during_open:
        env["PBS_MOCK_INFO_DELAY_MS"] = "450"
    if args.inject_error != "none":
        env["PBS_MOCK_FAIL_" + args.inject_error.upper()] = "1"
    if args.open_busy:
        env["PBS_MOCK_OPEN_BUSY_COUNT"] = "4" if args.open_busy == "recover" else "999999"
        env["PBS_MOCK_CAPTURE_SLOT_MASK"] = "1" if args.open_busy == "recover" else "0"
        if args.open_busy == "recover":
            env["PBS_MOCK_CAPTURE_SLOT_DYNAMIC"] = "1"
    command = [str(OUT / "mock-server"), "avserve", str(args.port),
               "seconds=9" if args.source_barrier or args.delayed_headers_ms or args.read_event_timeouts or args.starved_control or args.restart_burst_during_open or args.spurious_positive_control or args.idle_cpu else "seconds=6"]
    process = subprocess.Popen(command, env=env, cwd=ROOT)
    url = f"http://127.0.0.1:{args.port}"
    try:
        if args.open_busy == "exhaust":
            process.wait(timeout=10)
            log = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
            attempts = [int(x) for x in re.findall(r"MOCK OpenVideoEx transient 80a70008 attempt=(\d+)", log)]
            if (process.returncode == 0 or not (30 <= len(attempts) <= 150) or
                    "OpenVideoEx failure quarantined" not in log or
                    "Avcap encoder slots at final OpenVideoEx 80a70008: mask=00000000 slot0=0" not in log or
                    "MOCK totals open=0 stop=0 close=0 term=1" not in log or
                    "OpenVideoEx profile=19 -> 80a70008 handle=(nil)" not in log):
                raise RuntimeError("Retry budget must expire without allocating or overlapping Avcap handles\n" + log[-1300:])
            print(f"PASS FW11 OpenVideoEx persistent 80a70008: {len(attempts)} bounded retries; Term balanced")
            return
        if args.open_busy == "recover":
            config = wait_config(url, process)
            with output.open("wb") as target:
                target.write(b"\x00\x00\x00\x01" + bytes.fromhex(config["sps"]))
                target.write(b"\x00\x00\x00\x01" + bytes.fromhex(config["pps"]))
                pts = []
                pull_frames(url, target, 40, pts)
            process.wait(timeout=10)
            log = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
            attempts = [int(x) for x in re.findall(r"MOCK OpenVideoEx transient 80a70008 attempt=(\d+)", log)]
            if (process.returncode != 0 or attempts != [1,2,3,4] or
                    "OpenVideoEx transient 80a70008 retries=4" not in log or
                    "Avcap encoder slots at first OpenVideoEx 80a70008: mask=00000001 slot0=1" not in log or
                    "Avcap encoder slots after successful OpenVideoEx: mask=00000001 slot0=1" not in log or
                    "Avcap encoder slots after final Close: mask=00000000 slot0=0" not in log or
                    "MOCK totals open=1 stop=1 close=1 term=1" not in log):
                raise RuntimeError("Transient 80a70008 retry must start once and cleanup normally\n" + log[-1400:])
            print("PASS FW11 OpenVideoEx transient 80a70008: 4 retries, one Start, 40 HTTP frames, balanced cleanup")
            return
        if args.inject_error in ("open", "start", "info"):
            process.wait(timeout=16)
            log = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
            if args.inject_error == "open":
                expected = ("OpenVideoEx failure quarantined", "MOCK totals open=1 stop=0 close=0 term=1")
            elif args.inject_error == "start":
                expected = ("MOCK Start failed after GetInfo", "Avcap recovery incomplete after 3 attempts",
                            "MOCK totals open=4 stop=0 close=4 term=1")
            else:
                expected = ("Pre-Start GetInfo SPS=-1 PPS=-1", "Avcap recovery incomplete after 3 attempts",
                            "MOCK totals open=4 stop=0 close=4 term=1")
            if process.returncode == 0 or any(item not in log for item in expected):
                raise RuntimeError("Partial Avcap initialization must terminate and release handles\n" + log[-1400:])
            print(f"PASS PS4 partial {args.inject_error} failure bounded; no overlapping capture resources")
            return
        config = wait_config(url, process)
        if args.idle_cpu:
            if args.native_read_wait or not sys.platform.startswith("linux"):
                parser.error("--idle-cpu requires Linux and the default flags=1")
            for _ in range(100):
                prior = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
                if "MOCK ReadVideo empty-ring nonblocking polling" in prior:
                    break
                time.sleep(0.01)
            else:
                raise RuntimeError("PS4 reader did not enter empty-queue state")
            cpu_before = linux_process_cpu_ms(process.pid)
            sample_started = time.monotonic()
            time.sleep(1.25)
            cpu_ms = linux_process_cpu_ms(process.pid) - cpu_before
            elapsed_ms = (time.monotonic() - sample_started) * 1000
            if cpu_ms > elapsed_ms * 0.35:
                raise RuntimeError(f"Idle ReadVideo unexpectedly used {cpu_ms:.0f}ms CPU in {elapsed_ms:.0f}ms")
            for _ in range(250):
                prior = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
                if "MOCK open count=2" in prior and prior.count("MOCK Start accepted") >= 2:
                    break
                time.sleep(0.02)
            else:
                raise RuntimeError("Reader did not recover from prolonged nonblocking empty queue")
            # A native Start can precede the HTTP server's generation update.
            # Joining at that instant belongs to the OLD generation and
            # disconnects by design. Observe the settled next generation.
            time.sleep(0.25)
            with output.open("wb") as target:
                target.write(b"\x00\x00\x00\x01" + bytes.fromhex(config["sps"]))
                target.write(b"\x00\x00\x00\x01" + bytes.fromhex(config["pps"]))
                pts = []
                pull_frames(url, target, 40, pts)
            process.wait(timeout=12)
            log = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
            count_match = re.search(r"MOCK fast empty-ring reads=(\d+)", log)
            if (process.returncode != 0 or "ReadVideo stalled" not in log or
                    not count_match or int(count_match.group(1)) < 500 or
                    "MOCK totals open=2 stop=2 close=2 term=1" not in log):
                raise RuntimeError("Empty-ring CPU fixture failed restart/cleanup\n" + log[-1200:])
            print(f"PASS PS4 nonblocking idle read: {cpu_ms:.0f}ms CPU / {elapsed_ms:.0f}ms wall; "
                  "stall detected, one recovery, 40 delivered frames")
            return
        if args.starved_control or args.restart_burst_during_open or args.spurious_positive_control:
            # Ensure the HTTP request races ReadVideo after Start, not setup.
            target_log = ("MOCK repeated positive event wakes with no frames" if args.spurious_positive_control
                          else "MOCK ReadVideo starvation: first 50ms event timeout")
            for _ in range(100):
                prior = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
                if target_log in prior:
                    break
                time.sleep(0.01)
            else:
                raise RuntimeError("ReadVideo starvation never reached after Start")
            partial_client = None
            if args.spurious_partial_client:
                partial_client = socket.create_connection(("127.0.0.1", args.port), timeout=2)
                partial_client.settimeout(1)
                partial_client.sendall(b"GET /")
                time.sleep(0.04)
            stop_mode = args.starved_control == "stop" or args.spurious_positive_control == "stop"
            endpoint = "/stop" if stop_mode else "/capture/restart"
            begin = time.monotonic()
            request = urllib.request.Request(url + endpoint, data=b"", method="POST")
            with urllib.request.urlopen(request, timeout=2) as reply:
                if reply.status != 200:
                    raise RuntimeError("Control request rejected during native event wait")
            response_ms = (time.monotonic() - begin) * 1000
            if response_ms > 1000:
                raise RuntimeError(f"HTTP control blocked by ReadVideo for {response_ms:.0f}ms")
            if args.spurious_positive_control and args.native_read_wait:
                # This deliberately unsafe comparison replicates the FW11
                # whole-call loop without ever returning. No Stop/Close is
                # allowed from another thread; finally terminates the HOST
                # mock, not a console service or ELF.
                time.sleep(0.4)
                blocked_log = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
                if (process.poll() is not None or
                        "MOCK positive event wake count=64, zero frames" not in blocked_log or
                        "Stop ->" in blocked_log or "Avcap reopen attempt" in blocked_log):
                    raise RuntimeError("Native flags=0 model unexpectedly returned during repeated positive events")
                print("PASS unsafe native flags=0 reproduced: repeated successful wakes trap ReadVideo; "
                      "HTTP request received but native teardown never begins")
                return
            if args.restart_burst_during_open:
                for _ in range(250):
                    prior = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
                    if "MOCK open count=2" in prior:
                        break
                    time.sleep(0.01)
                else:
                    raise RuntimeError("First HTTP restart did not reach second Avcap Open")
                # A second user/client presses restart while the first
                # request is already being serviced, before new SPS/PPS.
                with urllib.request.urlopen(request, timeout=2) as reply:
                    if reply.status != 200:
                        raise RuntimeError("Concurrent duplicate restart rejected")
                process.wait(timeout=12)
                log = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
                if (process.returncode != 0 or log.count("MOCK open count=") != 2 or
                        "Coalesced duplicate HTTP restart received during Avcap reopen" not in log or
                        "MOCK totals open=2 stop=2 close=2 term=1" not in log):
                    raise RuntimeError("Duplicate restart during Open triggered an extra encoder session\n" + log[-1600:])
                print("PASS duplicate client restart during Open coalesced without extra Avcap reopen")
                return
            if args.starved_control == "restart" or args.spurious_positive_control == "restart":
                for _ in range(250):
                    prior = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
                    if "MOCK open count=2" in prior and prior.count("MOCK Start accepted") >= 2:
                        break
                    time.sleep(0.02)
                else:
                    raise RuntimeError("ReadVideo timeout prevented the requested Avcap reopen")
                output = OUT / "client.h264"
                with output.open("wb") as target:
                    target.write(b"\x00\x00\x00\x01" + bytes.fromhex(config["sps"]))
                    target.write(b"\x00\x00\x00\x01" + bytes.fromhex(config["pps"]))
                    pts = []
                    pull_frames(url, target, 40, pts)
            process.wait(timeout=12)
            log = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
            expected = ("MOCK totals open=1 stop=1 close=1 term=1" if stop_mode
                        else "MOCK totals open=2 stop=2 close=2 term=1")
            if (expected not in log or "ReadVideo stalled" in log or
                    (stop_mode and process.returncode != 1) or
                    log.count(target_log) != 1 or
                    ((args.starved_control == "restart" or args.spurious_positive_control == "restart") and
                     (process.returncode != 0 or "Restart requested by HTTP client" not in log))):
                raise RuntimeError("Starved reader failed to stop/restart cleanly\n" + log[-1600:])
            if partial_client:
                try:
                    if partial_client.recv(1):
                        raise RuntimeError("Partial HTTP client survived server stop")
                finally:
                    partial_client.close()
            print(f"PASS empty-ring ReadVideo with {'positive wakes' if args.spurious_positive_control else 'wait timeouts'}: "
                  f"HTTP {endpoint} responded in {response_ms:.1f}ms; Avcap handles balanced")
            return
        if args.inject_error == "publish":
            process.wait(timeout=10)
            log = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
            if (process.returncode == 0 or "H264 publish failed after 45 frames" not in log or
                "publish_failed=1" not in log or "MOCK totals open=1 stop=1 close=1 term=1" not in log):
                raise RuntimeError("PS4 publish error must fail, stop and release capture resources\n" + log[-1300:])
            print("PASS PS4 publish failure exits nonzero and releases Avcap instead of claiming streaming success")
            return
        if config.get("width") != 1280 or config.get("height") != 720:
            raise RuntimeError("Unexpected mock video profile")
        if config.get("audio") is not None:
            raise RuntimeError("PS4 mock must report unavailable audio")
        with output.open("wb") as target:
            target.write(b"\x00\x00\x00\x01" + bytes.fromhex(config["sps"]))
            target.write(b"\x00\x00\x00\x01" + bytes.fromhex(config["pps"]))
            first_pts = []
            pull_frames(url, target, 40, first_pts)
            if args.probe_empty:
                time.sleep(3.0)
            else:
                request = urllib.request.Request(url + "/capture/restart", data=b"", method="POST")
                with urllib.request.urlopen(request, timeout=2) as reply:
                    if reply.status != 200:
                        raise RuntimeError("Capture restart HTTP API failed")
            if args.inject_error != "none":
                process.wait(timeout=10)
                log = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
                if (process.returncode == 0 or "Avcap cleanup failed; refusing to reopen" not in log
                        or "MOCK totals open=1 stop=1 close=1 term=1" not in log):
                    raise RuntimeError("Failed Avcap teardown must abort reopen, report error and balance Term\n" + log[-1400:])
                print("PASS PS4 fatal " + args.inject_error + " failure aborted reopen without duplicating Avcap handles")
                return
            # The optional smoke-only barrier runs after every OpenVideoEx;
            # do not attach the second client to an encoder still awaiting
            # its asynchronous mode event and pre-Start GetInfo headers.
            time.sleep(0.45 + (1.65 if args.never_mode_event else
                               max(args.delayed_mode_ms, args.delayed_headers_ms) / 1000.0 + 0.4
                               if args.source_barrier or args.delayed_headers_ms else 0))
            second_pts = []
            pull_frames(url, target, 40, second_pts)
            if second_pts[0] <= first_pts[-1]:
                raise RuntimeError("PTS counter was reset after capture reopened")
        process.wait(timeout=10)
        if process.returncode:
            raise RuntimeError(f"PS4 mocked main failed with exit {process.returncode}")
        log = (OUT / "ps4" / "capture.txt").read_text(encoding="utf-8")
        if "Avcap reopen attempt 1/3" not in log or "MOCK totals open=2 stop=2 close=2 term=1" not in log:
            raise RuntimeError("Capture reopen/cleanup did not balance mock handles\n" + log[-1400:])
        if log.count("MOCK Start accepted: pre-Start GetInfo mask=3") != 2:
            raise RuntimeError("Native PS4 requires GetInfo(SPS/PPS) BEFORE each Start\n" + log[-1400:])
        if args.read_event_timeouts:
            timeouts = log.count("MOCK ReadVideo event wait timeout")
            if timeouts < 4:
                raise RuntimeError("Native wait-enabled ReadVideo did not survive simulated 50ms producer stalls")
        if args.delayed_headers_ms:
            info_waits = [int(x) for x in re.findall(
                r"Pre-Start GetInfo SPS=\d+ PPS=\d+ elapsed_ms=(\d+)", log)]
            if len(info_waits) != 2:
                raise RuntimeError("No native pre-Start header synchronization for both capture sessions")
            if not args.source_barrier and min(info_waits) < args.delayed_headers_ms - 25:
                raise RuntimeError(f"GetInfo returned prematurely: observed waits={info_waits}ms")
        if args.source_barrier:
            if args.never_mode_event:
                if log.count("no proven READY_SCANIN; Start proceeds for diagnostic capture") != 2:
                    raise RuntimeError("Missing mode event must time out without a false readiness claim")
            elif args.delayed_mode_ms:
                if "Source transition: observed output change then stable snapshot" not in log:
                    raise RuntimeError("Did not wait for asynchronous output status settle")
            elif "Source transition: observed output change then stable snapshot" not in log:
                raise RuntimeError("Immediate output transition not observed")
        # This stage exists only in observer-enabled smoke/mock builds. It
        # distinguishes a VideoOut transition during GetInfo from one after
        # Start; neither snapshot is a proof of valid hardware scan-in.
        if log.count("SceAvSetting output snapshot after Pre-Start GetInfo:") != 2:
            raise RuntimeError("Missing pre-Start output observations for both encoder sessions")
        if len(re.findall(
                r"Pre-Start GetInfo SPS=\d+ PPS=\d+ elapsed_ms=\d+\n"
                r"(?:.*\n)*?SceAvSetting output snapshot after Pre-Start GetInfo:"
                r"(?:.*\n)*?MOCK Start accepted: pre-Start GetInfo mask=3", log)) != 2:
            raise RuntimeError("Pre-Start output observation must occur after headers and before Start")
        for phase in ("after remote request", "after OpenVideoEx", "after Pre-Start GetInfo", "after Start",
                      "during ReadVideo", "after Stop", "before term"):
            if f"Avcap static-info(21) {phase}: rc=00000000 flags=00000003" not in log:
                raise RuntimeError(f"Missing valid capture-request state at '{phase}'")
            if f"ShellCore RemotePlay status {phase}: rc=00000000 value=0" not in log:
                raise RuntimeError(f"Missing ShellCore RemotePlay status observation at '{phase}'")
            expected_mode = {"after OpenVideoEx": 5, "after Pre-Start GetInfo": 5, "after Start": 6,
                             "during ReadVideo": 6, "after Stop": 5}.get(phase, 4)
            if (args.delayed_mode_ms or args.never_mode_event) and phase == "after OpenVideoEx":
                expected_mode = 4  # Mode update is asynchronous, after Open returns.
            if args.never_mode_event and phase == "after Stop":
                expected_mode = 4
            if args.never_mode_event and phase == "after Pre-Start GetInfo":
                expected_mode = 4
            if f"SceAvSetting output snapshot {phase}: rc=00000000 base_mode={expected_mode} raw40=" not in log:
                raise RuntimeError(f"Missing SceAvSetting output-mode observation at '{phase}'")
            expected_bus = 2 if phase in ("after OpenVideoEx", "after Pre-Start GetInfo", "after Start", "during ReadVideo", "after Stop") else 0
            if (args.delayed_mode_ms or args.never_mode_event) and phase == "after OpenVideoEx":
                expected_bus = 0
            if args.never_mode_event and phase == "after Stop":
                expected_bus = 0
            if args.never_mode_event and phase == "after Pre-Start GetInfo":
                expected_bus = 0
            enum = 0x0d if expected_bus else 0x0c
            if f"SceVideoOut bus snapshot {phase}: rc=00000000 enum_byte6={enum:02x} flags={expected_bus:08x} native_bit1={int(expected_bus == 2)} raw40=" not in log:
                raise RuntimeError(f"Missing/incorrect native VideoOut bus flags at '{phase}'")
        low_rate = "WARNING: unusually low H.264 output rate" in log
        if low_rate != args.expect_low_bitrate:
            raise RuntimeError(f"Unexpected low-bitrate diagnosis: observed={low_rate}, expected={args.expect_low_bitrate}")
        if args.probe_empty and log.count("SUSPECTED EMPTY SCAN-IN") != 1:
            raise RuntimeError("Smoke must automatically re-open the suspected empty scan-in exactly once")
        if args.expect_low_bitrate:
            print(f"PASS PS4 low-bitrate source detection/restart and observable AV mode transitions: {output}")
        else:
            print(f"PASS PS4 HTTP delivered 80 visible-source frames through restart and AV state transitions: {output}")
        print("Run tools/verify_ps4_capture.py to check actual decoded pixels.")
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)


if __name__ == "__main__":
    main()
