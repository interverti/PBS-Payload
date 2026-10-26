#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Install the payload SDK for one console and export its path to the job.
#
# Resolution order:
#   1. a preinstalled SDK in the job environment (self-hosted runners, .env);
#   2. a cached or previously installed tree in .sdk/<platform>;
#   3. an archive from SDK_URL;
#   4. a shallow clone of SDK_REPO built once, then cached by actions/cache.
set -euo pipefail

platform=${PLATFORM:?PLATFORM is required}
# The upstream candidates are only a default: a fork, a pinned tag or an
# internal mirror is selected with <PLATFORM>_PAYLOAD_SDK_REPO/_REF.
case "$platform" in
  ps4)
    variable=PS4_PAYLOAD_SDK
    wrapper=orbis-clang
    default_repo=https://github.com/ps4-payload-dev/sdk
    ;;
  ps5)
    variable=PS5_PAYLOAD_SDK
    wrapper=prospero-clang
    default_repo=https://github.com/ps5-payload-dev/sdk
    ;;
  *)
    echo "::error::unknown platform '$platform'; expected ps4 or ps5"
    exit 1
    ;;
esac

export_sdk() {
  local sdk=$1
  echo "Using the $platform payload SDK at $sdk"
  printf '%s=%s\n' "$variable" "$sdk" >> "$GITHUB_ENV"
  printf 'path=%s\n' "$sdk" >> "$GITHUB_OUTPUT"
}

# 1. A preinstalled SDK: the runner is expected to provide the whole tree.
current=${!variable:-}
if [[ -n ${current:-} && -x $current/bin/$wrapper ]]; then
  export_sdk "$current"
  exit 0
fi

target=$PWD/.sdk/$platform

# 2. A restored cache (or an earlier step in the same job).
if [[ -x $target/bin/$wrapper ]]; then
  export_sdk "$target"
  exit 0
fi

install_build_tools() {
  command -v cmake >/dev/null 2>&1 && command -v curl >/dev/null 2>&1 && return 0
  if command -v apt-get >/dev/null 2>&1; then
    echo "Installing the SDK build tools"
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq cmake ninja-build python3 curl unzip zlib1g-dev libssl-dev
  fi
}

# The build script may place the toolchain in a subdirectory; find the
# directory whose bin/ holds the compiler wrapper.
sdk_root() {
  local start=$1 found
  found=$(find "$start" -maxdepth 5 -path "*/bin/$wrapper" -type f -print -quit 2>/dev/null || true)
  if [[ -z ${found:-} ]]; then
    return 1
  fi
  dirname "$(dirname "$found")"
}

# 3. A prebuilt archive.
if [[ -n ${SDK_URL:-} ]]; then
  echo "Downloading the $platform payload SDK from $SDK_URL"
  archive=$RUNNER_TEMP/payload-sdk-$platform.archive
  curl -fsSL --retry 3 --retry-delay 2 -o "$archive" "$SDK_URL"
  mkdir -p "$target"
  case "$SDK_URL" in
    *.zip) unzip -q "$archive" -d "$target" ;;
    *.tar.gz | *.tgz) tar -xzf "$archive" -C "$target" ;;
    *.tar.xz | *.txz) tar -xJf "$archive" -C "$target" ;;
    *.tar) tar -xf "$archive" -C "$target" ;;
    *)
      if ! tar -xf "$archive" -C "$target" 2>/dev/null; then
        unzip -q "$archive" -d "$target"
      fi
      ;;
  esac
  root=$(sdk_root "$target" || true)
  if [[ -z ${root:-} ]]; then
    echo "::error::$SDK_URL does not contain bin/$wrapper"
    exit 1
  fi
  if [[ $root != "$target" ]]; then
    # Flatten the single top-level directory the archive was packed with.
    shopt -s dotglob
    mv "$root"/* "$target"/
    shopt -u dotglob
    rm -rf "$root"
  fi
  export_sdk "$target"
  exit 0
fi

# 4. Build the upstream SDK once; actions/cache stores the tree afterwards.
repo=${SDK_REPO:-$default_repo}
ref=${SDK_REF:-}
install_build_tools
echo "Cloning $repo${ref:+ ($ref)}"
clone_args=(--depth 1)
if [[ -n $ref ]]; then
  clone_args+=(--branch "$ref")
fi
if ! git clone "${clone_args[@]}" "$repo" "$target"; then
  extra=${variable%_SDK}
  echo "::error::cannot clone $repo. Either preinstall the SDK and export $variable,"
  echo "::error::or set the repository variable ${extra}_SDK_URL to a prebuilt archive,"
  echo "::error::or point ${extra}_SDK_REPO${ref:+ and ${extra}_SDK_REF} at a reachable SDK."
  exit 1
fi
build_script=
for candidate in build.sh build; do
  if [[ -x $target/$candidate ]]; then
    build_script=$target/$candidate
    break
  fi
done
if [[ -z $build_script ]]; then
  echo "::error::$repo has no executable build script; provide $variable or ${variable%_SDK}_SDK_URL instead"
  exit 1
fi
echo "Building the SDK with $build_script"
( cd "$target" && "$build_script" )

root=$(sdk_root "$target" || true)
if [[ -z ${root:-} ]]; then
  echo "::error::the SDK build finished without bin/$wrapper"
  exit 1
fi
export_sdk "$root"
