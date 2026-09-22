#!/usr/bin/env bash
set -euo pipefail

ne16_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
chipyard_root=$(cd "$ne16_root/../.." && pwd)

cd "$chipyard_root"
python3 "$ne16_root/scripts/test-import-dory-ne16-generated-tile.py"
python3 "$ne16_root/scripts/test-import-dory-ne16-generated-network.py"
cmake -S tests -B tests/build -D CMAKE_BUILD_TYPE=Debug
if [[ "${ONLY_NETWORK:-0}" == 1 ]]; then
  cmake --build tests/build --target ne16-dory-generated-network \
    --parallel "${JOBS:-2}"
  output_file=$(mktemp)
  echo "Running tests/build/ne16-dory-generated-network.riscv"
  if ! timeout "${TIMEOUT:-30m}" \
      sims/verilator/simulator-chipyard.harness-NE16RocketConfig \
      +permissive +loadmem=tests/build/ne16-dory-generated-network.riscv \
      +permissive-off tests/build/ne16-dory-generated-network.riscv 2>&1 |
      tee "$output_file"; then
    rm -f "$output_file"
    exit 1
  fi
  if ! grep -F "DORY generated three-layer NE16 network PASS" \
      "$output_file" >/dev/null; then
    rm -f "$output_file"
    exit 1
  fi
  rm -f "$output_file"
  exit 0
fi
for test in ne16-descriptor ne16-dma ne16-dory-generated-tile \
  ne16-dory-generated-layer0 ne16-dory-generated-layer1 \
  ne16-dory-generated-layer2 ne16-dory-generated-network \
  ne16-conv ne16-conv3x3 ne16-dory-layer; do
  cmake --build tests/build --target "$test" --parallel "${JOBS:-2}"
done

run_test() {
  local binary=$1
  local expected=$2
  local output_file
  output_file=$(mktemp)
  echo "Running $binary"
  if ! timeout "${TIMEOUT:-30m}" \
      sims/verilator/simulator-chipyard.harness-NE16RocketConfig \
      +permissive +loadmem="$binary" +permissive-off "$binary" 2>&1 |
      tee "$output_file"; then
    rm -f "$output_file"
    return 1
  fi
  if ! grep -F "$expected" "$output_file" >/dev/null; then
    rm -f "$output_file"
    return 1
  fi
  rm -f "$output_file"
}

for binary in \
  tests/build/ne16-descriptor.riscv \
  tests/build/ne16-dma.riscv \
  tests/build/ne16-conv.riscv \
  tests/build/ne16-conv3x3.riscv \
  tests/build/ne16-dory-layer.riscv; do
  case "$binary" in
    *ne16-descriptor*) expected="NE16 descriptor PASS" ;;
    *ne16-dma*) expected="NE16 DMA PASS" ;;
    *ne16-conv.riscv) expected="NE16 convolution PASS" ;;
    *ne16-conv3x3*) expected="NE16 3x3 convolution PASS" ;;
    *) expected="DORY NE16 layer PASS" ;;
  esac
  run_test "$binary" "$expected"
done

run_test tests/build/ne16-dory-generated-tile.riscv \
  "DORY generated NE16 tile PASS"
run_test tests/build/ne16-dory-generated-layer0.riscv \
  "DORY generated NE16 layer0 PASS"
run_test tests/build/ne16-dory-generated-layer1.riscv \
  "DORY generated NE16 layer1 PASS"
run_test tests/build/ne16-dory-generated-layer2.riscv \
  "DORY generated NE16 layer2 PASS"
run_test tests/build/ne16-dory-generated-network.riscv \
  "DORY generated three-layer NE16 network PASS"
