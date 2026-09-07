#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir="$root/build"
bender=${BENDER:-"$root/.tools/bender"}

if [[ ! -x "$bender" ]]; then
  echo "Bender not found at $bender; run scripts/fetch-bender.sh" >&2
  exit 1
fi

mkdir -p "$build_dir"
cd "$root"
"$bender" --local checkout >&2

hci_dir=$("$bender" path hci)
original_sink="$hci_dir/rtl/core/hci_core_sink.sv"
patched_sink="$build_dir/hci_core_sink.sv"
raw_manifest="$build_dir/ne16-verilator.raw.f"
manifest="$build_dir/ne16-verilator.f"

cp "$original_sink" "$patched_sink"
(
  cd "$build_dir"
  patch -p0 < "$root/patches/hci-fifo-interface.patch" >&2
)

"$bender" script verilator --top ne16_top_wrap > "$raw_manifest"
awk -v old="$original_sink" -v patched="$patched_sink" \
  '$0 == old { print patched; next } { print }' \
  "$raw_manifest" > "$manifest"
printf '%s\n' "$root/rtl/ne16_chipyard_wrapper.sv" >> "$manifest"

printf '%s\n' "$manifest"
