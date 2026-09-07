#!/usr/bin/env bash
set -euo pipefail

ne16_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
chipyard_root=$(cd "$ne16_root/../.." && pwd)

cd "$chipyard_root"
cmake -S tests -B tests/build -D CMAKE_BUILD_TYPE=Debug
for test in ne16-conv ne16-conv3x3; do
  cmake --build tests/build --target "$test" --parallel "${JOBS:-2}"
  timeout "${TIMEOUT:-30m}" \
    sims/verilator/simulator-chipyard.harness-NE16RocketConfig \
    "tests/build/$test.riscv"
done
