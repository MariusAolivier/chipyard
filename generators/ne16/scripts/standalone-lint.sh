#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
manifest=$("$root/scripts/prepare-rtl.sh")

verilator \
  --lint-only \
  --timing \
  --top-module ne16_top_wrap \
  -Wall \
  -Wno-BLKANDNBLK \
  -Wno-fatal \
  -f "$manifest"

echo "NE16_STANDALONE_LINT_PASS"
