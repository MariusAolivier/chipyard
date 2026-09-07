#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
upstream="$root/upstream"
build="$root/build-verilator"
bender=${BENDER:-/cluster/work/mariusao/tools/bender/v0.28.1/bender}
verilator_bin=${VERILATOR:-verilator}

mkdir -p "$build"
cd "$upstream"
"$bender" script verilator -t rtl -t ita_hwpe > "$build/ita-hwpe.f"

"$verilator_bin" \
  --binary \
  --timing \
  --top-module ita_hwpe_smoke_tb \
  -Wall \
  -Wno-fatal \
  -Wno-BLKANDNBLK \
  -f "$build/ita-hwpe.f" \
  "$root/rtl/ita_hwpe_smoke_tb.sv" \
  --Mdir "$build/obj-smoke" \
  -o ita-hwpe-smoke

echo "ITA_SMOKE_VERILATOR_BUILD_PASS"
