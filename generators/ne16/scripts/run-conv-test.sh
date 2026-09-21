#!/usr/bin/env bash
set -euo pipefail

ne16_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
chipyard_root=$(cd "$ne16_root/../.." && pwd)

cd "$chipyard_root"
python3 "$ne16_root/scripts/test-import-dory-ne16-generated-tile.py"
cmake -S tests -B tests/build -D CMAKE_BUILD_TYPE=Debug
for test in ne16-descriptor ne16-dma ne16-dory-generated-tile ne16-conv ne16-conv3x3 ne16-dory-layer; do
  cmake --build tests/build --target "$test" --parallel "${JOBS:-2}"
done

for binary in \
  tests/build/ne16-descriptor.riscv \
  tests/build/ne16-dma.riscv \
  tests/build/ne16-dory-generated-tile.riscv \
  tests/build/ne16-conv.riscv \
  tests/build/ne16-conv3x3.riscv \
  tests/build/ne16-dory-layer.riscv; do
  echo "Running $binary"
  timeout "${TIMEOUT:-30m}" sims/verilator/simulator-chipyard.harness-NE16RocketConfig \
    +permissive +loadmem="$binary" +permissive-off "$binary" || exit $?
done
