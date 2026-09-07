#!/usr/bin/env bash
set -euo pipefail

ne16_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
chipyard_root=$(cd "$ne16_root/../.." && pwd)
manifest=$("$ne16_root/scripts/prepare-rtl.sh")
rtl_sources=$(grep -v '^[+]' "$manifest" | tr '\n' ' ')

cd "$chipyard_root/sims/verilator"
make \
  CONFIG=NE16RocketConfig \
  EXTRA_SIM_REQS="$manifest $rtl_sources" \
  EXTRA_SIM_SOURCES="-f $manifest -Wno-BLKANDNBLK"
