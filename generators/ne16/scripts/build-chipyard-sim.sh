#!/usr/bin/env bash
set -euo pipefail

ne16_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
chipyard_root=$(cd "$ne16_root/../.." && pwd)
testchipip_root="$chipyard_root/generators/testchipip"
testchipip_patch="$ne16_root/patches/testchipip-simdram-valid-write-data.patch"

simdram_cc="$testchipip_root/src/main/resources/testchipip/csrc/SimDRAM.cc"
simdram_v="$testchipip_root/src/main/resources/testchipip/vsrc/SimDRAM.v"

if ! grep -Fq "(w_valid && mm->w_ready()) ? svSize(w_data, 1) : 0" "$simdram_cc" ||
   ! grep -Fq "__w_data[0:(DATA_BITS / 8)-1];" "$simdram_v" ||
   ! grep -Fq "__r_data[0:(DATA_BITS / 8)-1];" "$simdram_v"; then
  if git -C "$testchipip_root" apply --check "$testchipip_patch"; then
    git -C "$testchipip_root" apply "$testchipip_patch"
  else
    # The submodule may already contain one hunk from an earlier run.
    grep -Fq "(w_valid && mm->w_ready()) ? svSize(w_data, 1) : 0" "$simdram_cc" ||
      sed -i \
        's/int w_data_bytes = svSize(w_data, 1);/int w_data_bytes = (w_valid \&\& mm->w_ready()) ? svSize(w_data, 1) : 0;/' \
        "$simdram_cc"
    if ! grep -Fq "__w_data[0:(DATA_BITS / 8)-1];" "$simdram_v" ||
       ! grep -Fq "__r_data[0:(DATA_BITS / 8)-1];" "$simdram_v"; then
      sed -i \
        -e 's/byte        __w_data\[(DATA_BITS \/ 8)-1:0\];/byte        __w_data[0:(DATA_BITS \/ 8)-1];/' \
        -e 's/byte __r_data\[(DATA_BITS \/ 8)-1:0\];/byte __r_data[0:(DATA_BITS \/ 8)-1];/' \
        "$simdram_v"
    fi
  fi
fi

manifest=$("$ne16_root/scripts/prepare-rtl.sh")
rtl_sources=$(grep -v '^[+]' "$manifest" | tr '\n' ' ')
generated_dir="$chipyard_root/sims/verilator/generated-src/chipyard.harness.TestHarness.NE16RocketConfig/gen-collateral"

if [ -f "$generated_dir/SimDRAM.v" ]; then
  sed -i \
    -e 's/byte        __w_data\[(DATA_BITS \/ 8)-1:0\];/byte        __w_data[0:(DATA_BITS \/ 8)-1];/' \
    -e 's/byte __r_data\[(DATA_BITS \/ 8)-1:0\];/byte __r_data[0:(DATA_BITS \/ 8)-1];/' \
    "$generated_dir/SimDRAM.v"
fi

if [ -f "$generated_dir/SimDRAM.cc" ]; then
  sed -i \
    -e 's/int w_data_bytes = svSize(w_data, 1);/int w_data_bytes = (w_valid \&\& mm->w_ready()) ? svSize(w_data, 1) : 0;/' \
    "$generated_dir/SimDRAM.cc"
fi

cd "$chipyard_root/sims/verilator"
make \
  CONFIG=NE16RocketConfig \
  EXTRA_SIM_REQS="$manifest $rtl_sources" \
  EXTRA_SIM_SOURCES="-f $manifest -Wno-BLKANDNBLK"
