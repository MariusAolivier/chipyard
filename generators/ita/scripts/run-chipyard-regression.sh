#!/usr/bin/env bash
set -euo pipefail

ita_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
chipyard_root=$(cd "$ita_root/../.." && pwd)

cd "$chipyard_root"
cmake -S tests -B tests/build -D CMAKE_BUILD_TYPE=Debug
cmake --build tests/build --target ita-linear --parallel "${JOBS:-2}"
timeout "${TIMEOUT:-30m}" \
  sims/verilator/simulator-chipyard.harness-ITARocketConfig \
  tests/build/ita-linear.riscv
