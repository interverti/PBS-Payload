#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The release version: VERSION at the repository root is the single source of truth.

  release-version.py show           print the numeric version in VERSION
  release-version.py set 0.2.0      write a new numeric version into VERSION
  release-version.py check v0.2.0-beta.1
                                    validate a tag/version against VERSION and,
                                    under GitHub Actions, write it to $GITHUB_OUTPUT

The numeric part of a tag (before any -suffix) must equal VERSION, so the ELF
file names, the GitHub tag and the store version always agree. Prerelease
suffixes live on the tag only, so a beta does not move the numeric version.
"""
import argparse
import os
from pathlib import Path
import re
import sys

VERSION_FILE = Path(__file__).resolve().parents[1] / "VERSION"
NUMERIC = re.compile(r"^\d+\.\d+\.\d+$")
# Same grammar as the store's manifest validator (lib/store/release-ingestion.ts).
SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$")


def project_version(path: Path = VERSION_FILE) -> str:
    try:
        text = path.read_text(encoding="utf-8").strip()
    except FileNotFoundError:
        raise ValueError(f"{path} is missing; the release version lives there") from None
    if not NUMERIC.fullmatch(text):
        raise ValueError(f"{path} must hold a major.minor.patch version, found {text!r}")
    return text


def set_project_version(version: str, path: Path = VERSION_FILE) -> None:
    version = version.strip().removeprefix("v")
    if not NUMERIC.fullmatch(version):
        raise ValueError("VERSION holds major.minor.patch only; prerelease suffixes live on the tag")
    path.write_text(version + "\n", encoding="utf-8")


def channel_of(version: str) -> str:
    """stable for x.y.z, nightly for *-nightly*, beta for every other prerelease."""
    prerelease = SEMVER.match(version).group(4)
    if not prerelease:
        return "stable"
    return "nightly" if "nightly" in prerelease.lower() else "beta"


def resolve(raw: str, path: Path = VERSION_FILE) -> dict:
    version = raw.strip().removeprefix("v")
    match = SEMVER.match(version)
    if not match:
        raise ValueError(f"'{raw}' is not a semantic version (expected e.g. v0.2.0 or v0.2.0-beta.1)")
    numeric = ".".join(match.group(i) for i in (1, 2, 3))
    expected = project_version(path)
    if numeric != expected:
        raise ValueError(f"version {version} does not match {expected} in VERSION; "
                         f"run `python tools/release-version.py set {numeric}` and commit first")
    return {
        "version": version,
        "tag": f"v{version}",
        "numeric": numeric,
        "prerelease": "true" if match.group(4) else "false",
        "channel": channel_of(version),
        "release_name": f"PBS Payload v{version}",
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("show")
    commands.add_parser("set").add_argument("version")
    commands.add_parser("check").add_argument("tag")
    args = parser.parse_args(argv)
    try:
        if args.command == "show":
            print(project_version())
        elif args.command == "set":
            set_project_version(args.version)
            print(f"VERSION now at {project_version()}")
        else:
            info = resolve(args.tag)
            lines = [f"{key}={value}" for key, value in info.items()]
            print("\n".join(lines))
            if os.environ.get("GITHUB_OUTPUT"):
                with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as out:
                    out.write("\n".join(lines) + "\n")
    except ValueError as error:
        print(f"::error::{error}" if os.environ.get("GITHUB_ACTIONS") else f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
