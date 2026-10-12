if {[llength $argv] < 1 || [llength $argv] > 2 ||
    ([llength $argv] == 2 && [lindex $argv 1] ne "--repair-host-clock")} {
    error "Usage: vivado -mode batch -source resume-firesim-u250-vivado.tcl -tclargs <NE16-cl-directory> \[--repair-host-clock\]"
}
if {![info exists ::env(SLURM_JOB_ID)]} {
    error "Run synthesis and implementation through Slurm"
}
set repair_host_clock [expr {[llength $argv] == 2}]

set root_dir [file normalize [lindex $argv 0]]
set vivado_version [version -short]
set vivado_version_major [string range $vivado_version 0 3]
set top_level_name overall_fpga_top

source ${root_dir}/scripts/utils.tcl
source ${root_dir}/scripts/platform_env_2024.2.tcl
source ${root_dir}/scripts/au250.tcl
check_file_exists ${root_dir}/vivado_proj/firesim.xpr
check_file_exists ${root_dir}/design/ne16-vivado.tcl
open_project ${root_dir}/vivado_proj/firesim.xpr

if {![string equal -nocase [get_property PART [current_project]] $part] ||
    [get_property BOARD_PART [current_project]] ne $board_part ||
    [get_property TOP [current_fileset]] ne $top_level_name} {
    error "The existing project is not the expected U250 FireSim design"
}
set block_design ${root_dir}/vivado_proj/firesim.srcs/sources_1/bd/design_1/design_1.bd
open_bd_design $block_design
set host_frequency [get_property CONFIG.CLKOUT1_REQUESTED_OUT_FREQ [get_bd_cells clk_wiz_0]]
if {$host_frequency != 60} {
    error "Expected the existing 60 MHz host clock; found: $host_frequency"
}
set host_clock_source [get_property CONFIG.PRIM_SOURCE [get_bd_cells clk_wiz_0]]
if {$host_clock_source ne "No_buffer" && !$repair_host_clock} {
    error "The DDR-derived host clock must use PRIM_SOURCE No_buffer; rebuild or explicitly repair the completed project"
}
if {$repair_host_clock} {
    if {$host_clock_source eq "No_buffer"} {
        error "The host clock is already configured correctly; omit --repair-host-clock"
    }
    if {[get_property STATUS [get_runs impl_1]] ne "write_bitstream Complete!"} {
        error "Host-clock repair requires a completed task-owned implementation"
    }
    set clock_net [get_bd_nets -of_objects [get_bd_pins clk_wiz_0/clk_in1]]
    set clock_driver [get_bd_pins -of_objects $clock_net -filter {DIR == O}]
    if {[llength $clock_driver] != 1 ||
        $clock_driver ne [get_bd_pins ddr4_0/c0_ddr4_ui_clk]} {
        error "Expected the host clock wizard to be driven only by DDR0's UI clock; found: $clock_driver"
    }
    set snapshot_dir ${root_dir}/vivado_proj/pre_host_clock_repair_$::env(SLURM_JOB_ID)
    if {[file exists $snapshot_dir]} {
        error "Host-clock repair snapshot already exists: $snapshot_dir"
    }
    set snapshot_files [list \
        ${root_dir}/vivado_proj/firesim.bit \
        ${root_dir}/vivado_proj/firesim.mcs \
        ${root_dir}/vivado_proj/firesim.xpr \
        ${root_dir}/vivado_proj/reports \
        $block_design \
        ${root_dir}/vivado_proj/firesim.srcs/sources_1/bd/design_1/ip/design_1_clk_wiz_0_0/design_1_clk_wiz_0_0.xci]
    foreach path $snapshot_files {
        check_file_exists $path
    }
    file mkdir $snapshot_dir
    foreach path $snapshot_files {
        file copy $path $snapshot_dir
    }
    puts "PRE_HOST_CLOCK_REPAIR_SNAPSHOT $snapshot_dir"

    # The input is a DDR-generated clock, not an external primary clock pin.
    set_property CONFIG.PRIM_SOURCE No_buffer [get_bd_cells clk_wiz_0]
    validate_bd_design
    save_bd_design
    generate_target all [get_files $block_design]
    reset_runs [get_runs design_1_clk_wiz_0_0_synth_1]
    puts "HOST_CLOCK_SOURCE_REPAIRED_REIMPLEMENTATION_REQUIRED"
}

source ${root_dir}/design/ne16-vivado.tcl
update_compile_order -fileset sources_1
report_ip_status
set rpt_dir ${root_dir}/vivado_proj/reports
file mkdir $rpt_dir
source ${root_dir}/scripts/strategies/strategy_TIMING.tcl

foreach sourceFile {
    synthesis.tcl
    post_synth.tcl
    implementation.tcl
    post_impl.tcl
} {
    check_file_exists ${root_dir}/scripts/$sourceFile
    source ${root_dir}/scripts/$sourceFile
}

puts "NE16_U250_IMPLEMENTATION_FINISHED_TIMING_REVIEW_REQUIRED"
exit 0
