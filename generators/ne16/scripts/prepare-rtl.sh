#!/usr/bin/env bash
set -euo pipefail

[[ $# -le 1 ]] || {
  echo "Usage: prepare-rtl.sh [--verilator|--vivado|--fpga-sim]" >&2
  exit 1
}
case "${1:---verilator}" in
  --verilator) tool=verilator ;;
  --vivado) tool=vivado ;;
  --fpga-sim) tool=fpga-sim ;;
  *)
    echo "Usage: prepare-rtl.sh [--verilator|--vivado|--fpga-sim]" >&2
    exit 1
    ;;
esac

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir="$root/build"
if [[ "$tool" != verilator ]]; then
  build_dir="$build_dir/$tool"
fi
bender=${BENDER:-"$root/.tools/bender"}

if [[ ! -x "$bender" ]]; then
  echo "Bender not found at $bender; run scripts/fetch-bender.sh" >&2
  exit 1
fi

mkdir -p "$build_dir"
cd "$root"
"$bender" --local checkout >&2

hci_dir=$("$bender" path hci)
original_sink="$hci_dir/rtl/core/hci_core_sink.sv"
patched_sink="$build_dir/hci_core_sink.sv"
cp "$original_sink" "$patched_sink"
(
  cd "$build_dir"
  patch -p0 < "$root/patches/hci-fifo-interface.patch" >&2
)

if [[ "$tool" != verilator ]]; then
  ctrl_dir=$("$bender" path hwpe-ctrl)
  stream_dir=$("$bender" path hwpe-stream)
  storage="$root/fpga/ne16_storage.sv"
  storage_originals=(
    "$ctrl_dir/rtl/hwpe_ctrl_regfile_latch.sv"
    "$stream_dir/rtl/fifo/hwpe_stream_fifo_scm.sv"
    "$root/upstream/rtl/input_buffer/ne16_input_buffer_scm.sv"
    "$root/upstream/rtl/accumulator/ne16_accumulator_scm.sv"
  )
  for source in "$storage" "${storage_originals[@]}"; do
    [[ -s "$source" ]] || {
      printf 'Required FPGA storage source is missing: %s\n' "$source" >&2
      exit 1
    }
  done
fi

if [[ "$tool" == verilator ]]; then
  raw_manifest="$build_dir/ne16-verilator.raw.f"
  manifest="$build_dir/ne16-verilator.f"
  "$bender" script verilator --top ne16_top_wrap > "$raw_manifest"
  awk -v old="$original_sink" -v patched="$patched_sink" \
    '$0 == old { print patched; next } { print }' \
    "$raw_manifest" > "$manifest"
  printf '%s\n' "$root/rtl/ne16_chipyard_wrapper.sv" >> "$manifest"
elif [[ "$tool" == fpga-sim ]]; then
  raw_manifest="$build_dir/ne16-fpga.raw.f"
  manifest="$build_dir/ne16-fpga.f"
  "$bender" script verilator --no-default-target \
    -t fpga -t xilinx -t synthesis -t vivado -D SYNTHESIS \
    --top ne16_top_wrap > "$raw_manifest"
  awk -v sink="$original_sink" -v patched_sink="$patched_sink" \
    -v originals="$(printf '%s\n' "${storage_originals[@]}")" '
    BEGIN { split(originals, sources, "\n"); for (i in sources) expected[sources[i]] = 0 }
    $0 == sink { print patched_sink; next }
    $0 in expected { expected[$0]++; next }
    { print }
    END {
      for (source in expected)
        if (expected[source] != 1) {
          print "Expected exactly one original FPGA storage source: " source > "/dev/stderr"
          exit 1
        }
    }' "$raw_manifest" > "$manifest.tmp"
  printf '%s\n' "$storage" "$root/rtl/ne16_chipyard_wrapper.sv" >> "$manifest.tmp"
  mv "$manifest.tmp" "$manifest"
else
  raw_manifest="$build_dir/ne16-vivado.raw.tcl"
  manifest="$build_dir/ne16-vivado.tcl"
  "$bender" script vivado --no-simset --top ne16_top_wrap > "$raw_manifest"
  cat > "$manifest" <<'TCL'
set ne16_saved_defines [get_property verilog_define [current_fileset]]
set ne16_saved_includes [get_property include_dirs [current_fileset]]
TCL
  cat "$raw_manifest" >> "$manifest"
  printf 'set ne16_original_sink_path {%s}\nset ne16_patched_sink_path {%s}\nset ne16_wrapper_path {%s}\n' \
    "$original_sink" "$patched_sink" "$root/rtl/ne16_chipyard_wrapper.sv" >> "$manifest"
  printf 'set ne16_fpga_storage_path {%s}\n' "$storage" >> "$manifest"
  printf 'set ne16_original_storage_paths [list \\\n' >> "$manifest"
  printf '    {%s} \\\n' "${storage_originals[@]}" >> "$manifest"
  printf ']\n' >> "$manifest"
  cat >> "$manifest" <<'TCL'
foreach {ne16_property ne16_saved} [list \
    verilog_define $ne16_saved_defines include_dirs $ne16_saved_includes] {
  set ne16_values [list]
  foreach ne16_value [concat $ne16_saved [get_property $ne16_property [current_fileset]]] {
    if {[lsearch -exact $ne16_values $ne16_value] < 0} {
      lappend ne16_values $ne16_value
    }
  }
  set_property $ne16_property $ne16_values [current_fileset]
}
set ne16_original_sink [get_files -quiet $ne16_original_sink_path]
if {[llength $ne16_original_sink] != 1} {
  error "Expected exactly one original HCI sink source; found: $ne16_original_sink"
}
remove_files $ne16_original_sink
add_files -norecurse $ne16_patched_sink_path
add_files -norecurse $ne16_wrapper_path
foreach path $ne16_original_storage_paths {
  set original [get_files -quiet $path]
  if {[llength $original] != 1} {
    error "Expected exactly one original clock-gated storage source: $path"
  }
  remove_files $original
}
add_files -norecurse $ne16_fpga_storage_path
set ne16_fpga_storage_selected 1
TCL
fi

printf '%s\n' "$manifest"
