#!/usr/bin/env bash
set -euo pipefail

[[ $# == 1 ]] || {
  echo "Usage: test-fpga-storage.sh <fresh-output-directory>" >&2
  exit 1
}
[[ ! -e "$1" ]] || {
  echo "The FPGA storage test output directory must be fresh: $1" >&2
  exit 1
}
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
chipyard_root=$(cd "$root/../.." && pwd)
mkdir -p "$1"
output=$(cd "$1" && pwd)
ulimit -c 0

python3 - "$chipyard_root/tests/ne16-dory-generated/dory_generated_tile_data.c" "$output" <<'PY'
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text()
for name, filename, count in (
    ("dory_tile_input", "input.hex", 1024),
    ("dory_tile_weights", "weights.hex", 2432),
    ("dory_tile_expected", "expected.hex", 1024),
):
    match = re.search(r"\b" + name + r"\[\d+\]\s*=\s*\{([^}]+)\};", source)
    if match is None:
        raise ValueError(f"Missing canonical fixture: {name}")
    values = [int(value.strip(), 0) for value in match[1].split(",") if value.strip()]
    if len(values) != count or any(not 0 <= value <= 255 for value in values):
        raise ValueError(f"Invalid canonical fixture: {name}")
    (pathlib.Path(sys.argv[2]) / filename).write_text(
        "".join(f"{value:02x}\n" for value in values)
    )
PY

for mode in asic fpga; do
  if [[ "$mode" == asic ]]; then
    manifest=$(bash "$root/scripts/prepare-rtl.sh")
  else
    manifest=$(bash "$root/scripts/prepare-rtl.sh" --fpga-sim)
  fi
  cp "$manifest" "$output/$mode.f"
  for test in storage4 storage8 core; do
    kind=storage
    options=("-GWIDTH_FACTOR=${test#storage}")
    [[ "$mode" != fpga ]] || options+=("-GREAD_WRITE_COLLISIONS=1")
    if [[ "$test" == core ]]; then
      kind=core
      options=()
    fi
    top="ne16_fpga_${kind}_tb"
    build="$output/${mode}_${test}"
    if ! verilator --binary --timing -j "${NE16_TEST_JOBS:-4}" \
      -Wno-fatal -Wno-BLKANDNBLK +define+SYNTHESIS \
      --top-module "$top" --Mdir "$build" \
      "${options[@]}" -f "$manifest" "$root/tests/ne16-fpga-${kind}-tb.sv" \
      > "$output/${mode}_${test}_build.log" 2>&1; then
      cat "$output/${mode}_${test}_build.log" >&2
      exit 1
    fi
    cases=("$test")
    [[ "$test" != core ]] || cases=(1x1 1x1-stalled 3x3 3x3-stalled dory dory-stalled)
    for case_name in "${cases[@]}"; do
      args=()
      case "$case_name" in
        1x1-stalled) args=(+backpressure) ;;
        3x3) args=(+conv3x3) ;;
        3x3-stalled) args=(+conv3x3 +backpressure) ;;
        dory) args=(+dory-fixture "+fixture-dir=$output") ;;
        dory-stalled) args=(+dory-fixture "+fixture-dir=$output" +backpressure) ;;
      esac
      log="$output/${mode}_${case_name}.log"
      if ! timeout --foreground --kill-after=5s 2m "$build/V$top" "${args[@]}" \
        > "$log" 2>&1; then
        cat "$log" >&2
        exit 1
      fi
      printf '%s/%s: ' "$mode" "$case_name"
      grep -E '^(STORAGE_PASS|CORE_PASS)' "$log"
    done
  done
done
echo "FPGA_STORAGE_AND_CORE_REGRESSION_PASS"
