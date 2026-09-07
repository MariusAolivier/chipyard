#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build="$root/build-verilator"
bender=${BENDER:-"$root/.tools/bender"}

if [[ ! -x "$bender" ]]; then
  echo "Bender not found at $bender; run scripts/fetch-bender.sh" >&2
  exit 1
fi

mkdir -p "$build"
cd "$root/upstream"
"$bender" script --local verilator -t rtl -t ita_hwpe \
  > "$build/ita-verilator.raw.f"

common_cells=$("$bender" path common_cells)
overlay="$build/common-cells-overlay"
rm -rf "$overlay"
mkdir -p "$overlay/src" "$overlay/include/ita_common_cells"
sed 's/COMMON_CELLS_REGISTERS_SVH_/ITA_COMMON_CELLS_REGISTERS_SVH_/g' \
  "$common_cells/include/common_cells/registers.svh" \
  > "$overlay/include/ita_common_cells/registers.svh"

while IFS= read -r source; do
  destination="$overlay/src/$(basename "$source")"
  sed 's#"common_cells/registers.svh"#"ita_common_cells/registers.svh"#g' \
    "$source" > "$destination"
done < <(
  grep -R -l 'common_cells/registers.svh' "$common_cells/src" --include='*.sv'
)

{
  printf '%s\n' \
    "$root/rtl/ita_common_cells_macro_reset.sv" \
    "+incdir+$overlay/include"
  while IFS= read -r line; do
    candidate="$overlay/src/$(basename "$line")"
    if [[ "$line" == "$common_cells"/src/* && -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
    else
      printf '%s\n' "$line"
    fi
  done < "$build/ita-verilator.raw.f"
} > "$build/ita-verilator.f"

printf '%s\n' "$root/rtl/ita_chipyard_wrapper.sv" \
  >> "$build/ita-verilator.f"
printf '%s\n' "$build/ita-verilator.f"
