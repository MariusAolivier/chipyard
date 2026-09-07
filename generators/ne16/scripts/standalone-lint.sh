#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
manifest=$("$root/scripts/prepare-rtl.sh")

verilator \
  --lint-only \
  --timing \
  --top-module NE16BlackBox \
  -Wall \
  -Wno-BLKANDNBLK \
  -Wno-fatal \
  -f "$manifest"

echo "NE16_STANDALONE_LINT_PASS"
