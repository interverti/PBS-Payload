# Changelog

Notable changes to PBS Payload. `VERSION` at the repository root holds the
numeric version; a release tag may add a suffix (`v0.2.0-beta.1`).

## v0.4.0-beta.2 — 2026-11-07

Beta, pre-release. Changes since `v0.4.0-beta.1`: PS5 capture now runs through
a detached worker and does not retry a rejected `OpenVideo` in the same payload
execution; the virtual controller survives viewer reconnects until shutdown.

### PS5 capture

- Boot a private RPC worker inside `SceRemotePlay`, then detach ptrace. The
  bootstrap resolves its exports by name, allocates its own RW mapping and
  switches it to RX; it uses no firmware offsets and does not overwrite
  executable padding. Sustained capture is refused if the worker cannot start.
  Shutdown sends the worker exit command, joins it and releases the mapping.
- Quarantine a failed `OpenVideo`. A successful `STOP`/`ClearCallback`/
  `Terminate` sequence proves only that PBS released its public objects, not
  that the backend freed the encoder slot, so a retry could keep returning
  `0x80c78000`. The payload now reopens capture only after it owned and closed
  a real video handle in the same execution.
- Remove the automatic 1080p retry after a `0x81950008` rejection. For the
  automatic 1440p profile on 10.01 (`0x10010000`), start at 1080p before the
  first open instead; explicit 1440p and 4K requests are unchanged.
- Find and validate the native `SceRemotePlay` request in target memory for the
  requested geometry, feature, usage, resolution and frame-rate mode. This
  release logs the result in `inject.txt` with reuse disabled: the payload
  still builds its synthetic request.
- Record the deterministic constructor writes for all 24 exact PS5 profiles as
  per-field initializers, with the generator refusing dynamic or conflicting
  stores. The catalogue is tested offline; it is not applied to the synthetic
  request in this release.
- Treat ResourceArbitrator `0x819b0007` as an already-registered callback:
  continue with a borrowed registration and, if PBS's RUNNING transition
  succeeds, balance it with STOP without clearing another owner's callback.

### Controller

- Keep the virtual PS5 controller device for the payload lifetime. A viewer
  disconnect only ends the session; `connect` reuses the existing device and
  `pbs_input_shutdown()` performs the final delete after the input thread has
  stopped. This removes the repeated `AddDevice`/`DeleteDevice` churn that
  exhausted the `libScePad` device pool across reconnects. Process privilege is
  retained until the same shutdown.
- Use the positive module handle returned by `sceKernelLoadStartModule()`
  directly. This fixes firmwares such as 10.60 where `libScePad` loads as
  `0x2023` but an immediate lookup by basename still fails.
- Quarantine a virtual device after `InsertData` fails. Reconnect is disabled
  for that payload execution, while final shutdown can still delete the known
  device ID without blind retries or pool churn.

### Worker and tooling

- Raise the worker RPC copy limit to 2 MiB and use inline byte-copy helpers in
  the relocated remote blob, so large frame reads do not depend on compiler
  helpers outside the mapping.
- Add the PS5 worker, request-template matcher and shared PS4 worker sources to
  both build scripts.
- Extend the offline suite with PS5 worker boot/shutdown, 24-profile request
  signature and initializer checks, capture quarantine tests and persistent
  controller lifecycle tests.
- Preserve and quarantine worker state after ambiguous `pthread_create`,
  unreadable thread handles, uncertain RPC exit or failed cleanup. A second
  bootstrap cannot overwrite a possibly live mapping, and late boot failures
  no longer cause a second detach in the caller.

### Known limits

- The PS5 worker bootstrap and native request matcher are covered by host-side
  tests; a console smoke test remains the final step before publishing the build.

## v0.2.0-beta.1 — 2026-10-29

Beta, pre-release. First release of the 0.2.0 line: both console payloads build
from this tree and the PS5 side ships 20 measured firmware profiles.

### Assets

| Asset | Platform | Loaded on |
| --- | --- | --- |
| `PBS-Payload-0.2.0-beta.1-ps5.elf` | PS5 | PS5 |
| `PBS-Payload-0.2.0-beta.1-ps4.elf` | PS4 | PS4 11.00 worker backend |

Each asset ships with a `.sha256` sidecar and `SHA256SUMS.txt` covers the whole
version.

### PS5 firmware coverage

Every profile is disassembled from a memory dump of the running console: the
Avcap2 request geometry comes from `libSceAvcap2.sprx.prefix.bin`, the
RemotePlay constructor from the `SceRemotePlay` EBOOT. Nothing is guessed from a
neighbouring version.

| Release | Firmware | Request | `kind` |
| --- | --- | --- | --- |
| 3.20 | `0x03200004` | `0x218` | `0xe8` |
| 4.00 | `0x04000042` | `0x230` | `0x100` |
| 4.51 | `0x04510001` | `0x230` | `0x100` |
| 6.02 | `0x06020004` | `0x2a8` | `0x168` |
| 7.20 | `0x07200005` | `0x2a8` | `0x168` |
| 8.40 | `0x08400005` | `0x2a8` | `0x168` |
| 9.20 | `0x09200005` | `0x2b0` | `0x168` |
| 9.40 | `0x09400008` | `0x2b0` | `0x168` |
| 10.01 | `0x10010000` | `0x2b8` | `0x188` |
| 10.60 | `0x10600006` | `0x2b8` | `0x188` |
| 11.20 | `0x11200005` | `0x2e0` | `0x1a0` |
| 11.40 | `0x11400005` | `0x2e0` | `0x1a0` |
| 11.60 | `0x11600005` | `0x2e0` | `0x1a0` |
| 12.00 | `0x12000043` | `0x2e0` | `0x1a0` |
| 12.40 | `0x12400009` | `0x2e0` | `0x1a0` |
| 12.70 | `0x12700001` | `0x2e0` | `0x1a0` |
| 13.00 | `0x13000040` | `0x2e0` | `0x1a0` |
| 13.20 | `0x13200006` | `0x2e0` | `0x1a0` |
| 13.42 | `0x13420000` | `0x2e8` | `0x1a8` |
| 13.60 | `0x13600007` | `0x2e8` | `0x1a8` |

Between the measured anchors, releases whose Avcap2 geometry is unchanged are
served by an audited family range instead of being refused:

| Family | Anchor | Releases accepted without their own profile |
| --- | --- | --- |
| 6.02–8.40 | `602` | 6.50, 7.00, 7.01, 7.40, 7.60, 7.61, 8.00, 8.20 |
| 9.20–9.40 | `920` | — |
| 10.01–10.60 | `1001` | 10.20, 10.40 |
| 11.40–12.70 | `1160` | 12.02, 12.20, 12.60 |
| 13.40–13.60 | `1360` | 13.40 |

Deliberately refused until a dump exists: **9.00**, **9.60**, **11.00**,
**14.00** and **14.10**. A wrong request size is not a soft failure, so an
unknown geometry never borrows another release's. 3.20, 4.00, 11.20, 13.00 and
13.20 are also exact-only: another build of the same release is refused rather
than served with a geometry it was never measured on.

### PS5 dump and ledger tooling

- `payload/build.ps1 -Platform PS5 -Kind dump` builds the dump payload
  (`build/pbs-ps5-dump.elf`) that saves the module images a profile is derived
  from, including `libSceAvcap2.sprx.prefix.bin` and the `SceRemotePlay` EBOOT;
  `payload/build.sh ps4 dump` produces the PS4 offset report the PS4 ledger is
  filled from.
- `tools/generate_ps5_offsets.py` turns a dump archive into a profile, reports
  archives whose name disagrees with the firmware they contain, and refuses
  ambiguous or conflicting images instead of picking one.
- `tools/data/ps5_firmware_offsets.json` records the evidence behind every
  profile; `tools/ps5_firmware_table.py --check` fails when the ledger, the
  profiles, `registry.c`, the family ranges and the public release catalog drift
  apart. `--list`, `--unknown`, `--compare` and `--write` inspect and regenerate
  the tables in `docs/PS5_FIRMWARE_OFFSETS.txt`.

### PS4

- Remote AVCap worker backend: reads video through exported
  `sceVshAvcapReadVideo`, no private ring offsets.
- Dynamic layout: kernel gate addresses are discovered from executable ELF
  segments and checked instruction context, the trampoline from live import
  padding; missing or ambiguous layouts are refused before any patch is applied.
- Diagnostics: admission probe, latch bypass, budget unlock, VENC journal,
  daemon dump, capture snapshot and privacy mask.
- 6.72 diagnostic support.
- Per-firmware offset table (`tools/data/ps4_firmware_offsets.json`) where an
  unmeasured field stays unmeasured, so a zero can never pass for an offset.

### Streaming and viewer

- H.264 capture with 720p, 1080p and 1440p profiles in 60 fps mode; the PS5
  picks 1080p at runtime for an observed 1080p output mode and explicit
  arguments override that choice.
- HTTP streaming with RTP packetization, capture restart and discontinuity
  handling.
- The HTTP server no longer blocks the start when its default port is taken: it
  scans the next ten ports and logs which one it bound.
- A stalled viewer no longer poisons the RTP stream: the sender detects a
  backlog, asks for a keyframe and resumes from an IDR.
- Periodic keyframe refresh and improved jitter recovery.
- Built-in browser viewer and dashboard; the owner banner is opt-in
  (`PBS_WEB_LOGO=1`) and off by default, so `/banner.png` answers 404 in a
  default build.

### Build, CI and release

- `release.yml`: validate the version against `VERSION`, build both payloads,
  verify every downloaded asset against its `.sha256`, register the version in
  the Seregon Store and publish the GitHub release. A run without `publish`
  stops at a draft.
- `verify.yml`: offline PS4 regression on the host plus optional PS4/PS5
  cross-builds.
- SDK provisioning without developer-specific paths: a preinstalled SDK, a
  repository archive variable, or a shallow clone of the upstream SDK.
- PS5 ELF validation (`tools/verify_ps5_elf.py`) runs on every build.

### Known limits

- PS4 hardware capture is validated on 11.00 at 720p only (180 s sessions,
  ~59.9 fps, decoded recording). Other PS4 firmwares remain unverified and PS4
  audio is disabled.
- PS5 profiles are verified by offline disassembly and bounds checks; a console
  smoke test is still the last step for each release. 4.00 was promoted from
  experimental to measured on 2026-10-29 after its dump was re-checked
  instruction by instruction, and it has the same remaining step.
- Native 4K, 120 fps and zero latency are not supported claims. The 60 Mbit/s
  encoder budget is a ceiling, not a guaranteed bitrate.
