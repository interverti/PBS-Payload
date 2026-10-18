# Project structure

The source tree separates shared services from console-specific code. Directory
names identify the platform and subsystem, so filenames do not repeat `ps4_`
or `h264_` prefixes. Existing C symbols, ABI layouts, feature switches, CLI
arguments, HTTP endpoints and ELF output names remain unchanged.

```text
payload/
  src/
    common/
      http/           HTTP server, stream delivery and recording
      media/          Video buffer helpers and H.264 RTP packetizer
      process/        Shared process injection interface and implementation
    platform/
      ps4/
        main.c        PS4 entry point and capture orchestration
        capture/      Local/remote Avcap, requests, ring and source state
        system/       Firmware profiles, credentials, gates and trampoline
        worker/       RPC client, target loop, bootstrap and wire layouts
        diagnostics/  Discovery, snapshots, admission and bounded experiments
      ps5/
        main.c        PS5 entry point, capture and audio orchestration
  web/                Viewer source, browser muxer and banner
  build/generated/    Embedded viewer and banner headers (ignored)
tests/
  http/               HTTP lifecycle, recording and frame-rate regressions
  media/              RTP and browser muxer regressions
  ps4/
    capture/          Capture behavior, mock source and transport regressions
    diagnostics/      Diagnostic ownership, bounds and rollback regressions
    system/           Firmware, control and trampoline regressions
    worker/           Bootstrap, shared-memory and socket regressions
  stubs/              Host substitutes for console SDK headers
  test_*.py           Capture verifier and journal inspector regressions
tools/                Build verification, host tests and console utilities
```

## Includes and dependencies

Quoted production includes are qualified from `payload/src`, for example
`common/http/server.h` or `platform/ps4/worker/client.h`. Both build entry
points add that single source root to the include path. Avoid adding every
subdirectory as an include path: `capture/remote.h` and `worker/remote.h`
have different owners and should never resolve by search order.

The shared process implementation retains its existing platform selection
through `PBS_PS4`; moving it does not change where console-specific code is
compiled. Firmware addresses stay in `platform/ps4/system/firmware.h`.
Diagnostic implementations are linked only by the existing target and feature
selection rules in `payload/build.sh`.

`payload/embed_assets.py` generates `webui.h` and `banner.h` under
`payload/build/generated`; the PowerShell build emits the same headers there.
HTTP compilation needs both `-Ipayload/src` and
`-Ipayload/build/generated`. Build scripts and host runners supply these paths
automatically. A clean checkout does not require generated source files.

Several capture scenario tests include the sibling `remote_capture.c` test
harness after renaming its entry point. That relative include intentionally
refers to test fixtures; the production remote implementation is linked
separately.

## Verification

Run these from the repository root:

```sh
bash tools/test_ps4_host.sh
node tests/media/h264_mux_rate.js
python -m unittest discover -s tests
```

The host runner requires a Linux C compiler, C++20 compiler and Python 3.
Node.js exercises the browser muxer; FFmpeg and FFprobe enable decoder tests.
Run the Python suite on a host with both decoder tools to avoid skipped cases.
The host runner also exercises the real assembly worker prototype and RTP
packetizer. All these checks are offline and require no console connection.

For a visible-video integration test, provide an existing Annex B fixture:

```sh
python3 tools/test_ps4_mock.py path/to/visible.h264
python3 tools/verify_ps4_capture.py build/ps4-mock/client.h264
```

Console build commands and deployment procedures are in [BUILDING.md](../BUILDING.md).
