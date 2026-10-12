if {[llength $argv] != 1 && [llength $argv] != 2} {
    error "Usage: vivado -mode batch -source firesim-u250-vivado-preflight.tcl -tclargs <board-repository> [NE16-source-manifest]"
}

set ::env(NE16_U250_BOARD_REPO) [lindex $argv 0]
source [file join [file dirname [info script]] .. fpga platform_env_2024.2.tcl]

set board_part xilinx.com:au250:part0:1.3
create_project -in_memory ne16_u250_preflight -part xcu250-figd2104-2l-e
set_property board_part $board_part [current_project]
puts "VIVADO_VERSION [version -short]"
puts "BOARD_PART [get_property BOARD_PART [current_project]]"

foreach ip {
    aurora_64b66b
    axi_clock_converter
    axi_dwidth_converter
    axis_clock_converter
    axis_data_fifo
    clk_wiz
    ddr4
    proc_sys_reset
    util_vector_logic
    util_ds_buf
    xdma
    xlconstant
} {
    set definitions [get_ipdefs -all xilinx.com:ip:${ip}:*]
    if {[llength $definitions] == 0} {
        error "Required IP is not installed: $ip"
    }
    puts "AVAILABLE_IP $ip $definitions"
}

if {[llength $argv] == 2} {
    set manifest [file normalize [lindex $argv 1]]
    if {![file isfile $manifest]} {
        error "NE16 source manifest does not exist: $manifest"
    }
    unset -nocomplain ne16_fpga_storage_selected
    source $manifest
    if {![info exists ne16_fpga_storage_selected] || $ne16_fpga_storage_selected != 1} {
        error "The NE16 FPGA explicit-enable storage overlay is required"
    }
    set original_files [lsort [get_files -quiet]]
    set original_defines [get_property verilog_define [current_fileset]]
    set original_includes [get_property include_dirs [current_fileset]]
    source $manifest
    if {[lsort [get_files -quiet]] ne $original_files ||
        [get_property verilog_define [current_fileset]] ne $original_defines ||
        [get_property include_dirs [current_fileset]] ne $original_includes} {
        error "Repeated NE16 source loading changed files, defines, or include directories"
    }
    synth_design -rtl -top NE16BlackBox
    foreach {port count} {tcdm_req_o 9 tcdm_gnt_i 9 tcdm_add_o 288 tcdm_data_o 288} {
        set expression [format {^%s\[[0-9]+\]$} $port]
        set actual [llength [get_ports -regexp $expression]]
        if {$actual != $count} {
            error "NE16 port $port has $actual bits; expected $count"
        }
    }
    puts "NE16_RTL_ELABORATION_PASSED_NOT_A_BITSTREAM_OR_MAC_RESULT"
    close_design
}

close_project
puts "U250_CATALOG_PREFLIGHT_PASSED_NOT_A_SYNTHESIS_RESULT"
exit 0
