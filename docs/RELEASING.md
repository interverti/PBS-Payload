# Releasing PBS Payload

Every published payload comes from a manual workflow run: nothing here builds
on a push or a tag, so a release never spends Actions minutes by accident and a
version is only created on purpose.

| Workflow | Purpose |
| --- | --- |
| `.github/workflows/verify.yml` | Offline PS4 regression on the host (no console) and, optionally, both payload cross-builds. |
| `.github/workflows/release.yml` | Builds the payloads, verifies them, creates the GitHub release and registers the version in the Seregon Store. |

## What a release publishes

| Asset | Built from | Loaded on |
| --- | --- | --- |
| `PBS-Payload-<version>-ps5.elf` | `payload/build/ps5/pbs.elf` | PS5 |
| `PBS-Payload-<version>-ps4.elf` | `payload/build/ps4/pbs-remote-stream.elf` (`PBS_PS4_REMOTE_AVCAP=1`) | PS4 11.00 worker backend |

Each asset is uploaded with its own `.sha256` sidecar, and `SHA256SUMS.txt`
lists every payload in the version. The GitHub release is created as a draft
first: without the `publish` switch the run stops with a draft to inspect and
nothing reaches the store.

The store receives the same two files with `platform`, `operating_system` set
to `console` and `architecture` to `x64`, because both consoles are x86-64 and
the launcher picks a payload from those three fields.

## One-time setup

1. **Version.** `VERSION` at the repository root holds `major.minor.patch` and
   is the single source of truth. Change it with
   `python tools/release-version.py set 0.2.0`; the tag may add a suffix
   (`v0.2.0-beta.1`) without moving the numeric version.
2. **Payload SDK.** The workflow provisions the SDK by itself, in this order:
   1. a preinstalled SDK in the job environment (`PS4_PAYLOAD_SDK` or
      `PS5_PAYLOAD_SDK`), for a self-hosted runner whose `.env` already defines
      it — pass the runner label with the `runs_on` input;
   2. an archive from the repository variables `PS4_PAYLOAD_SDK_URL` /
      `PS5_PAYLOAD_SDK_URL` (optionally `PS4_PAYLOAD_SDK_REPO`,
      `PS4_PAYLOAD_SDK_REF`, same for PS5);
   3. otherwise a shallow clone of the upstream SDK (by default
      `ps4-payload-dev/sdk` and `ps5-payload-dev/sdk`; point
      `PS4_PAYLOAD_SDK_REPO` / `PS5_PAYLOAD_SDK_REPO` and their `_REF` at a
      fork, a pinned tag or an internal mirror when those names do not match
      your source), installed with `make DESTDIR=<private-prefix> install` for the official SDKs
      (or their executable build script for custom repositories), then cached by `actions/cache` under
      `.sdk/<platform>` (the Makefile installation is in `.installed/`);
      the installer hash also invalidates old cache entries. Bump `cache-key` in
      `.github/actions/payload-sdk/action.yml` to force a rebuild.
3. **Store credential.** Add the repository secret `RELEASE_CI_TOKEN`: the CI
   token of `https://store.seregonwar.com`, at least 32 characters. The store
   accepts the shared token for every linked repository, and a dedicated token
   per repository when `RELEASE_CI_REPO_TOKENS` is configured.
4. **Store product link.** `/api/ci/releases` resolves the product from the
   repository name, so `seregonwar/PBS-Payload` must be linked to the payload
   product in the store database (`product_repositories`). The catalog entry
   and the link are seeded by the store's own migrations; without the link the
   first presign answers `404 Repository is not linked to a product.`

## Release procedure

1. Bump and commit the version when the numeric part changes:

   ```powershell
   python tools/release-version.py set 0.2.0
   git commit -am 'Release 0.2.0'
   git push
   ```

2. Run **Actions → Release** with the version, the platforms and `publish`
   left off.
3. Inspect the draft release: the two `.elf` assets, their `.sha256` sidecars,
   `SHA256SUMS.txt`, and the manifest printed by the *Store manifest (dry run)*
   step. The dry run shows the exact presign and register bodies, including the
   storage paths the store will record.
4. Run the workflow again with the same version and `publish` enabled. It
   reuses the draft, re-uploads the same bytes, registers the version in the
   store and finally leaves the draft state. A stable version becomes the
   store's latest release; `beta` and `nightly` stay off `latest`.
5. Verify from the store: open the product page, download both payloads and
   compare them with the release checksums.

## Version and channel rules

| Version | GitHub | Store |
| --- | --- | --- |
| `0.2.0` | tag `v0.2.0`, latest | channel `stable`, published as latest |
| `0.2.0-beta.1` | tag `v0.2.0-beta.1`, prerelease | channel `beta` |
| `0.2.0-nightly.7` | tag `v0.2.0-nightly.7`, prerelease | channel `nightly` |

The store rejects a prerelease on the stable channel and refuses to change
metadata of an already published file: a released version is immutable, so a
mistake is fixed by publishing a new version. Registering the same version
again is safe — the store skips files whose path is already recorded and
re-hashes the rest.

## Local dry runs

The tooling works without the store and without a token:

```powershell
python tools/release-version.py check v0.2.0-beta.1
python tools/store-release.py --dir payload/build/ps4 --version 0.2.0 --channel beta --platforms ps4 --dry-run
```

The unit tests exercise the same rules the store enforces (filename grammar,
2 GiB cap, semantic version, stable without a suffix, one file per name):

```powershell
python -m unittest discover -s tests -p test_store_release.py
```

## Failure guide

| Symptom | Cause and fix |
| --- | --- |
| `version X does not match Y in VERSION` | The numeric part of the requested version differs from `VERSION`; run `release-version.py set` and commit. |
| `vX is already published` | The tag exists and is not a draft. Published files are immutable: bump `VERSION`. |
| `missing PBS-Payload-X-ps4.elf` | A platform was selected that the run did not build; pass the matching `platforms` value. |
| `Repository is not linked to a product` | The store has no product link for this repository; apply the catalog migration. |
| `RELEASE_CI_TOKEN is missing or not valid` | The repository secret is absent, shorter than 32 characters or not the store's token. |
| `Uploaded files do not match the manifest` | The artifact changed after hashing. Rebuild the payload instead of editing an asset. |
| `cannot tell the platform` / store rejects a file | The filename must stay `PBS-Payload-<version>-<platform>.elf`: letters, digits, `.`, `_`, `+`, `-` and spaces only. |
| `the SDK build finished without bin/<wrapper>` | The upstream build changed its layout; set `PS4_PAYLOAD_SDK_URL` / `PS5_PAYLOAD_SDK_URL` to a prebuilt tree. |

## Manual fallback

When Actions is unavailable, the same steps can run locally on Linux or WSL:

```bash
PS4_PAYLOAD_SDK=/path/to/ps4-payload-sdk PBS_PS4_REMOTE_AVCAP=1 bash payload/build.sh ps4 stream
PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk bash payload/build.sh ps5 stream
mkdir -p assets
install -m 0644 payload/build/ps4/pbs-remote-stream.elf assets/PBS-Payload-0.2.0-ps4.elf
install -m 0644 payload/build/ps5/pbs.elf assets/PBS-Payload-0.2.0-ps5.elf
RELEASE_CI_TOKEN=... python3 tools/store-release.py --dir assets --version 0.2.0 --channel stable --publish
```

Upload the same files to a GitHub release by hand if the workflow could not
create it.
