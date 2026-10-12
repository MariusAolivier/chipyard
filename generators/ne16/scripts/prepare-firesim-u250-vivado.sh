#!/usr/bin/env bash
set -euo pipefail

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

[[ $# == 1 ]] || die "Usage: bash prepare-firesim-u250-vivado.sh <fresh-NE16-cl-directory>"
cl_dir=$(cd "$1" && pwd)
[[ $(basename "$cl_dir") != cl_firesim ]] ||
  die "Prepare a generated build directory, not the FireSim source template"
[[ -f "$cl_dir/stamp" && -s "$cl_dir/design/FireSim-generated.sv" ]] ||
  die "Generate the NE16 RTL with make replace-rtl before preparing Vivado"
grep -q NE16DMA "$cl_dir/design/FireSim-generated.sv" ||
  die "Generated RTL does not contain the current NE16 DMA"

ne16_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
library="$cl_dir/scripts/bd_lib"
patch_file="$ne16_root/patches/firesim-u250-vivado-2024.2.patch"
platform_file="$ne16_root/fpga/platform_env_2024.2.tcl"

[[ ! -e "$library/2024.2" && ! -L "$library/2024.2" ]] ||
  die "Vivado 2024.2 definitions already exist; refusing to overwrite them"
[[ ! -e "$cl_dir/scripts/platform_env_2024.2.tcl" &&
   ! -L "$cl_dir/scripts/platform_env_2024.2.tcl" ]] ||
  die "A Vivado 2024.2 platform overlay already exists"
[[ -s "$patch_file" && -s "$platform_file" ]] ||
  die "NE16 Vivado overlay files are missing"

for file in create_aurora_exdes.tcl create_bd_connections.tcl \
            create_bd_instances.tcl create_bd_interfaces.tcl ip_mod_list.tcl; do
  [[ -s "$library/2023.1/$file" ]] ||
    die "Required FireSim block-design definition is missing: $file"
done

mkdir "$library/2024.2"
for file in create_bd_connections.tcl create_bd_interfaces.tcl; do
  ln -s "../2023.1/$file" "$library/2024.2/$file"
done
for file in create_aurora_exdes.tcl create_bd_instances.tcl ip_mod_list.tcl; do
  cp -L "$library/2023.1/$file" "$library/2024.2/$file"
done

patch --batch --forward --fuzz=0 --dry-run -p1 -d "$cl_dir" -i "$patch_file"
patch --batch --forward --fuzz=0 -p1 -d "$cl_dir" -i "$patch_file"
cp "$platform_file" "$cl_dir/scripts/platform_env_2024.2.tcl"
manifest=$(bash "$ne16_root/scripts/prepare-rtl.sh" --vivado)
[[ -s "$manifest" ]] || die "The NE16 Vivado source manifest was not generated"
cp "$manifest" "$cl_dir/design/ne16-vivado.tcl"

printf '%s\n' "Prepared the U250 Vivado 2024.2 overlay; synthesis and implementation remain required."
