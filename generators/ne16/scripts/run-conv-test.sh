#!/usr/bin/env bash
set -euo pipefail

ne16_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
chipyard_root=$(cd "$ne16_root/../.." && pwd)

cd "$chipyard_root"
cmake -S tests -B tests/build -D CMAKE_BUILD_TYPE=Debug
cmake --build tests/build --target ne16-conv --parallel "${JOBS:-2}"
timeout "${TIMEOUT:-30m}" \
  sims/verilator/simulator-chipyard.harness-NE16RocketConfig \
  tests/build/ne16-conv.riscv
