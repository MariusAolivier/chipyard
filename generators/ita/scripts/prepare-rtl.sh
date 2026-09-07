#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build="$root/build-verilator"
bender=${BENDER:-"$root/.tools/bender"}

if [[ ! -x "$bender" ]]; then
  echo "Bender not found at $bender; run scripts/fetch-bender.sh" >&2
  exit 1
fi

mkdir -p "$build"
cd "$root/upstream"
"$bender" script --local verilator -t rtl -t ita_hwpe \
  > "$build/ita-verilator.f"
printf '%s\n' "$root/rtl/ita_chipyard_wrapper.sv" \
  >> "$build/ita-verilator.f"
printf '%s\n' "$build/ita-verilator.f"
