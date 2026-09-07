#!/usr/bin/env bash
set -euo pipefail

ita_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
chipyard_root=$(cd "$ita_root/../.." && pwd)
manifest=$("$ita_root/scripts/prepare-rtl.sh")
rtl_sources=$(grep -v '^[+]' "$manifest" | tr '\n' ' ')

cd "$chipyard_root/sims/verilator"
make \
  CONFIG=ITARocketConfig \
  EXTRA_SIM_REQS="$manifest $rtl_sources" \
  EXTRA_SIM_SOURCES="-f $manifest -Wno-BLKANDNBLK"
