#!/usr/bin/env bash
set -euo pipefail

version=0.28.1
archive="bender-$version-x86_64-linux-gnu.tar.gz"
sha256=561de10e4108627f5accdd1fed94380456440d99f6766f7750fbbe1903e53b68
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tools="$root/.tools"
binary="$tools/bender-$version/bender"

if [[ ! -x "$binary" ]]; then
  mkdir -p "$tools/bender-$version"
  curl --retry 8 --retry-all-errors --connect-timeout 30 -fL \
    "https://github.com/pulp-platform/bender/releases/download/v$version/$archive" \
    -o "$tools/$archive"
  printf '%s  %s\n' "$sha256" "$tools/$archive" | sha256sum -c -
  tar -xzf "$tools/$archive" -C "$tools/bender-$version"
fi

ln -sfn "$binary" "$tools/bender"
printf '%s\n' "$tools/bender"
