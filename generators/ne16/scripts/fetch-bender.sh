#!/usr/bin/env bash
set -euo pipefail

version=v0.32.1
archive=bender-x86_64-unknown-linux-gnu.tar.xz
sha256=59a36723b056a06b266dc68d4ceedcd0aa17a1c096e8c2ea512af264ff6a13f6
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tools_dir="$root/.tools"
install_dir="$tools_dir/bender-$version"
binary="$install_dir/bender-x86_64-unknown-linux-gnu/bender"

if [[ ! -x "$binary" ]]; then
  mkdir -p "$install_dir"
  curl --retry 8 --retry-all-errors --connect-timeout 30 -fL \
    "https://github.com/pulp-platform/bender/releases/download/$version/$archive" \
    -o "$tools_dir/$archive"
  printf '%s  %s\n' "$sha256" "$tools_dir/$archive" | sha256sum -c -
  tar -xJf "$tools_dir/$archive" -C "$install_dir"
fi

ln -sfn "$binary" "$tools_dir/bender"
printf '%s\n' "$tools_dir/bender"
