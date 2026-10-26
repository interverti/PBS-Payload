#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Register a PBS Payload release in the Seregon Store.

The store ingests CI releases in two steps over POST /api/ci/releases:

  1. {"action": "presign", repoFullName, version, files:[{filename, size_bytes}]}
     answers with one presigned R2 PUT address per file that is not registered
     yet, plus the storage path the store will record;
  2. {"action": "register", ...} records the whole manifest in one transaction.
     The store re-reads and re-hashes every new object and rejects a manifest
     whose bytes differ, so a version can never be half published.

Nothing becomes visible unless --publish is given: the version is registered
with is_published=false and an administrator or a later publish flips it.

Local checks mirror the store's own validator (lib/store/release-ingestion.ts):
filename grammar, the 2 GiB cap, the semantic version grammar, one file per
name and no prerelease on the stable channel. A bad manifest fails before any
byte is uploaded.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import urllib.error
import urllib.request

DEFAULT_STORE = "https://store.seregonwar.com"
DEFAULT_SLUG = "pbs-payload"
DEFAULT_REPO = "seregonwar/PBS-Payload"
ENDPOINT = "/api/ci/releases"
TOKEN_ENV = "RELEASE_CI_TOKEN"

TARGETS = ("ps4", "ps5")
ARTIFACT_PREFIX = "PBS-Payload-"
CHANNELS = ("stable", "beta", "nightly")

# The store's manifest validator: /^[A-Za-z0-9][A-Za-z0-9_.+ -]*$/, at most 200 characters.
FILENAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.+ -]{0,199}$")
VERSION = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$")
MAX_FILE_BYTES = 2147483648
MAX_NOTES = 20000

# The store classifies .elf/.bin/.self files as console payloads; both consoles
# are x86-64 and the launcher selects a payload by these three fields.
CONSOLE_PLATFORM = {"platform": "console", "operating_system": "console", "architecture": "x64"}


class ReleaseError(RuntimeError):
    """A manifest, artifact or store answer that must stop the release."""


def artifact_name(version: str, target: str) -> str:
    return f"{ARTIFACT_PREFIX}{version}-{target}.elf"


def parse_targets(raw: str) -> tuple:
    targets = [item.strip().lower() for item in raw.replace(" ", ",").split(",") if item.strip()]
    unknown = sorted(set(targets) - set(TARGETS))
    if unknown:
        raise ReleaseError(f"unknown platform(s) {', '.join(unknown)}; expected {', '.join(TARGETS)}")
    if not targets:
        raise ReleaseError("no platform selected")
    return tuple(dict.fromkeys(targets))


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect(directory: str, version: str, targets: tuple) -> list:
    """The release files for the selected platforms, checked against the store rules."""
    root = Path(directory)
    if not root.is_dir():
        raise ReleaseError(f"{root} does not exist; download the release assets first")
    wanted = {artifact_name(version, target): target for target in targets}
    found = {}
    for path in sorted(root.iterdir()):
        # Only ELF payloads are signed into the manifest; .sha256 sidecars and
        # the SHA256SUMS.txt list stay on the GitHub release.
        if not path.is_file() or path.suffix.lower() != ".elf":
            continue
        if path.name not in wanted:
            raise ReleaseError(
                f"{path.name} is not an artifact of version {version}; "
                f"publish only what this run built (selected: {', '.join(targets)})")
        if not FILENAME.fullmatch(path.name):
            raise ReleaseError(f"{path.name} has characters the store rejects (allowed: letters, digits, . _ + - and space)")
        size = path.stat().st_size
        if size <= 0:
            raise ReleaseError(f"{path.name} is empty")
        if size > MAX_FILE_BYTES:
            raise ReleaseError(f"{path.name} is larger than the store's 2 GiB limit")
        found[path.name] = {
            "filename": path.name,
            "path": path,
            "target": wanted[path.name],
            "size_bytes": size,
            "sha256": sha256_of(path),
        }
    missing = sorted(set(wanted) - set(found))
    if missing:
        raise ReleaseError(
            f"missing {', '.join(missing)}; build it with `bash payload/build.sh "
            f"{wanted[missing[0]]} stream` before publishing")
    return [found[name] for name in sorted(found)]


def check_release(version: str, channel: str, files: list) -> None:
    if not VERSION.fullmatch(version):
        raise ReleaseError(f"'{version}' is not a semantic version (expected e.g. 0.2.0 or 0.2.0-beta.1)")
    if channel not in CHANNELS:
        raise ReleaseError(f"channel must be one of {', '.join(CHANNELS)}")
    if channel == "stable" and "-" in version:
        raise ReleaseError(f"prerelease {version} cannot use the stable channel")
    names = [item["filename"].lower() for item in files]
    if len(names) != len(set(names)):
        raise ReleaseError("duplicate release file")


def platform_of(target: str) -> dict:
    if target not in TARGETS:
        raise ReleaseError(f"unknown platform {target}")
    return dict(CONSOLE_PLATFORM)


def storage_paths_for(files: list, slug: str, version: str) -> dict:
    """The path the store records: <product slug>/<version>/<filename>."""
    return {item["filename"]: f"{slug}/{version}/{item['filename']}" for item in files}


def presign_body(files: list, repo: str, version: str) -> dict:
    return {
        "action": "presign",
        "repoFullName": repo,
        "version": version,
        "files": [{"filename": item["filename"], "size_bytes": item["size_bytes"]} for item in files],
    }


def register_body(files: list, repo: str, version: str, channel: str, storage_paths: dict,
                  notes: str = "", publish: bool = False, run_id: str = "") -> dict:
    entries = []
    for item in files:
        entry = {
            "filename": item["filename"],
            "storage_path": storage_paths[item["filename"]],
            "sha256": item["sha256"],
            "size_bytes": item["size_bytes"],
            "storage_provider": "r2",
        }
        entry.update(platform_of(item["target"]))
        entries.append(entry)
    body = {
        "action": "register",
        "repoFullName": repo,
        "version": version,
        "channel": channel,
        "is_published": bool(publish),
        "artifact_name": f"{ARTIFACT_PREFIX}{version}",
        "files": entries,
    }
    if notes:
        body["release_notes"] = notes[:MAX_NOTES]
    if run_id:
        body["workflow_run_id"] = str(run_id)[:120]
    return body


def post_json(url: str, body: dict, token: str = "", timeout: int = 120) -> dict:
    raw = json.dumps(body).encode("utf-8")
    request = urllib.request.Request(url, data=raw, method="POST",
                                     headers={"Content-Type": "application/json"})
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = response.read().decode("utf-8") or "{}"
    return json.loads(payload)


def put_file(url: str, path: Path, timeout: int = 600) -> None:
    """Upload one artifact to its presigned address. Only the body may change."""
    data = path.read_bytes()
    request = urllib.request.Request(url, data=data, method="PUT")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        if response.status not in (200, 201, 204):
            raise ReleaseError(f"{path.name}: upload answered {response.status}")


def endpoint_of(store: str) -> str:
    return store.rstrip("/") + ENDPOINT


def explain_http_error(error: urllib.error.HTTPError, repo: str) -> str:
    try:
        detail = error.read().decode("utf-8", "replace")
    except Exception:  # noqa: BLE001 - the body is only informative here
        detail = ""
    try:
        parsed = json.loads(detail)
        message = parsed.get("error") or detail
        extra = parsed.get("details")
        if isinstance(extra, list) and extra:
            message = f"{message} ({'; '.join(str(item) for item in extra)})"
    except ValueError:
        message = detail or error.reason
    hints = {
        401: "RELEASE_CI_TOKEN is missing or not valid for this repository",
        404: f"repository {repo} is not linked to a store product; add the catalog link first",
        409: "a published version cannot change; bump VERSION and publish a new version",
        503: "store CI ingestion or R2 storage is not configured on the server",
    }
    hint = hints.get(error.code)
    return f"{error.code} {message}" + (f" — {hint}" if hint else "")


def read_notes(args) -> str:
    notes = ""
    if args.notes_file:
        notes = Path(args.notes_file).read_text(encoding="utf-8")
    if args.notes:
        notes = args.notes if not notes else f"{args.notes}\n\n{notes}"
    return notes.strip()[:MAX_NOTES]


def summary(files: list) -> str:
    lines = []
    for item in files:
        mib = item["size_bytes"] / (1024 * 1024)
        lines.append(f"  {item['filename']:<44} {item['target']:<4} {mib:6.2f} MiB  {item['sha256']}")
    return "\n".join(lines)


def run(args) -> int:
    targets = parse_targets(args.platforms)
    files = collect(args.dir, args.version, targets)
    check_release(args.version, args.channel, files)
    notes = read_notes(args)
    guessed_paths = storage_paths_for(files, args.slug, args.version)
    presign = presign_body(files, args.repo, args.version)
    register = register_body(files, args.repo, args.version, args.channel, guessed_paths,
                             notes, args.publish, os.environ.get("GITHUB_RUN_ID", ""))
    print(f"release {args.version} ({args.channel}) for {args.repo}")
    print(summary(files))

    if args.dry_run:
        print("\npresign request:")
        print(json.dumps(presign, indent=2))
        print("\nregister request (storage paths as the store computes them):")
        print(json.dumps(register, indent=2))
        return 0

    token = os.environ.get(TOKEN_ENV, "")
    if not token:
        raise ReleaseError(f"{TOKEN_ENV} is not set; the store rejects an anonymous manifest")
    url = endpoint_of(args.store)
    try:
        answer = post_json(url, presign, token)
    except urllib.error.HTTPError as error:
        raise ReleaseError(explain_http_error(error, args.repo)) from None
    except urllib.error.URLError as error:
        raise ReleaseError(f"cannot reach {url}: {error.reason}") from None

    presigned = {entry.get("filename"): entry for entry in answer.get("files", [])}
    missing = [item["filename"] for item in files if item["filename"] not in presigned]
    if missing:
        raise ReleaseError(f"the store did not answer for {', '.join(missing)}")

    storage = dict(guessed_paths)
    for item in files:
        entry = presigned[item["filename"]]
        # The store owns the storage layout; trust its answer when it has one.
        if entry.get("storage_path"):
            storage[item["filename"]] = entry["storage_path"]
        if entry.get("exists"):
            print(f"already registered: {item['filename']}")
            continue
        try:
            put_file(entry["upload_url"], item["path"])
        except urllib.error.HTTPError as error:
            raise ReleaseError(f"{item['filename']}: upload failed with "
                               f"{explain_http_error(error, args.repo)}") from None
        except urllib.error.URLError as error:
            raise ReleaseError(f"{item['filename']}: upload failed: {error.reason}") from None
        print(f"uploaded: {item['filename']}")

    register["files"] = register_body(files, args.repo, args.version, args.channel, storage,
                                      notes, args.publish, os.environ.get("GITHUB_RUN_ID", ""))["files"]
    try:
        result = post_json(url, register, token)
    except urllib.error.HTTPError as error:
        raise ReleaseError(explain_http_error(error, args.repo)) from None
    except urllib.error.URLError as error:
        raise ReleaseError(f"cannot reach {url}: {error.reason}") from None

    product = result.get("product") or {}
    print(f"registered {args.version} (channel {args.channel}, published: {str(args.publish).lower()})")
    if product.get("slug"):
        print(f"store product: {product['slug']}")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dir", required=True, help="directory holding the release artifacts")
    parser.add_argument("--version", required=True, help="release version, e.g. 0.2.0 or 0.2.0-beta.1")
    parser.add_argument("--channel", default="stable", choices=CHANNELS)
    parser.add_argument("--platforms", default=",".join(TARGETS), help="comma separated subset of ps4,ps5")
    parser.add_argument("--notes", default="", help="release notes sent to the store")
    parser.add_argument("--notes-file", default="", help="file with the release notes")
    parser.add_argument("--publish", action="store_true", help="publish the version in the store")
    parser.add_argument("--dry-run", action="store_true", help="print both requests without touching the store")
    parser.add_argument("--slug", default=os.environ.get("PBS_STORE_SLUG", DEFAULT_SLUG),
                        help="product slug used when the store does not return a storage path")
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", DEFAULT_REPO))
    parser.add_argument("--store", default=os.environ.get("PBS_STORE_URL", DEFAULT_STORE))
    args = parser.parse_args(argv)
    try:
        return run(args)
    except ReleaseError as error:
        prefix = "::error::" if os.environ.get("GITHUB_ACTIONS") else "error: "
        print(f"{prefix}{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
