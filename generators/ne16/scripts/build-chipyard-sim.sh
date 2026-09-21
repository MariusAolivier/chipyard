#!/usr/bin/env bash
set -euo pipefail

ne16_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
chipyard_root=$(cd "$ne16_root/../.." && pwd)
testchipip_root="$chipyard_root/generators/testchipip"
testchipip_patch="$ne16_root/patches/testchipip-simdram-valid-write-data.patch"

if ! grep -Fq "(w_valid && mm->w_ready()) ? svSize(w_data, 1) : 0" \
  "$testchipip_root/src/main/resources/testchipip/csrc/SimDRAM.cc"; then
  git -C "$testchipip_root" apply --check "$testchipip_patch"
  git -C "$testchipip_root" apply "$testchipip_patch"
fi

manifest=$("$ne16_root/scripts/prepare-rtl.sh")
rtl_sources=$(grep -v '^[+]' "$manifest" | tr '\n' ' ')

cd "$chipyard_root/sims/verilator"
make \
  CONFIG=NE16RocketConfig \
  EXTRA_SIM_REQS="$manifest $rtl_sources" \
  EXTRA_SIM_SOURCES="-f $manifest -Wno-BLKANDNBLK"
