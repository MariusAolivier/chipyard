if {[llength $argv] != 1} {
    error "Usage: vivado -mode batch -source review-firesim-u250-vivado.tcl -tclargs <NE16-cl-directory>"
}
if {![info exists ::env(SLURM_JOB_ID)]} {
    error "Run the implementation review through Slurm"
}

set root_dir [file normalize [lindex $argv 0]]
source ${root_dir}/scripts/platform_env_2024.2.tcl
set report_dir ${root_dir}/vivado_proj/review_$::env(SLURM_JOB_ID)
if {[file exists $report_dir]} {
    error "Review reports already exist: $report_dir"
}
file mkdir $report_dir

open_project ${root_dir}/vivado_proj/firesim.xpr
set run [get_runs impl_1]
puts "IMPLEMENTATION_STATUS [get_property STATUS $run]"
puts "IMPLEMENTATION_PROGRESS [get_property PROGRESS $run]"
if {[get_property STATUS $run] ne "write_bitstream Complete!" ||
    [get_property PROGRESS $run] ne "100%"} {
    error "Expected a completed implementation and bitstream"
}
foreach property {STATS.WNS STATS.TNS STATS.WHS STATS.THS STATS.TPWS} {
    set value [get_property $property $run]
    puts "$property $value"
    if {![string is double -strict $value] || $value < 0} {
        error "Implementation timing failed: $property = $value"
    }
}

open_run impl_1
set ne16_roots [get_cells -quiet -hierarchical -filter {NAME =~ */accelerator/i_ne16}]
if {[llength $ne16_roots] != 1} {
    error "Expected exactly one mapped NE16 hierarchy: $ne16_roots"
}
set ne16_root [get_property NAME $ne16_roots]
set storage_report [open ${report_dir}/ne16_storage.rpt w]
foreach module {hwpe_ctrl_regfile_latch hwpe_stream_fifo_scm ne16_input_buffer_scm ne16_accumulator_scm} {
    set instances [get_cells -quiet -hierarchical -filter \
        "NAME =~ $ne16_root/* && (REF_NAME =~ $module* || ORIG_REF_NAME =~ $module*)"]
    puts $storage_report "STORAGE_MODULE $module INSTANCES [llength $instances]"
    foreach instance $instances {
        puts $storage_report "STORAGE_INSTANCE $instance"
    }
    if {[llength $instances] == 0} {
        if {$module ne "hwpe_stream_fifo_scm"} {
            close $storage_report
            error "No mapped instances found for FPGA storage module $module"
        }
        puts "STORAGE_MODULE_NOT_RETAINED $module; checking all NE16 primitives and clocks"
    }
}
set primitives [get_cells -quiet -hierarchical -filter \
    "NAME =~ $ne16_root/* && IS_PRIMITIVE"]
set latches [filter $primitives {REF_NAME =~ LD*}]
puts $storage_report "NE16_LATCH_COUNT [llength $latches]"
if {[llength $latches] != 0} {
    puts $storage_report "NE16_LATCHES $latches"
    close $storage_report
    error "Mapped NE16 contains storage latches"
}
set clock_pins [get_pins -quiet -of_objects $primitives -filter \
    {IS_CLOCK == 1}]
if {[llength $clock_pins] == 0} {
    close $storage_report
    error "No mapped NE16 clock pins found"
}
set tied_pins [filter $clock_pins {IS_TIED == 1}]
set checked_dsps [dict create]
foreach pin $tied_pins {
    set owner [get_property NAME [get_cells -of_objects $pin]]
    while {$owner ne $ne16_root &&
           [get_property REF_NAME [get_cells $owner]] ne "DSP48E2"} {
        set owner [file dirname $owner]
    }
    if {$owner eq $ne16_root} {
        close $storage_report
        error "Tied NE16 clock outside a DSP multiplier: $pin"
    }
    if {[dict exists $checked_dsps $owner]} {
        continue
    }
    dict set checked_dsps $owner 1
    set dsp [get_cells $owner]
    foreach property {AREG ACASCREG BREG BCASCREG MREG PREG ALUMODEREG CARRYINREG CARRYINSELREG INMODEREG OPMODEREG} {
        if {[get_property $property $dsp] ne "0"} {
            close $storage_report
            error "Tied DSP clock has an active pipeline: $owner $property"
        }
    }
    if {[get_property USE_MULT $dsp] ne "MULTIPLY" ||
        [get_property AMULTSEL $dsp] ne "A" ||
        [get_property BMULTSEL $dsp] ne "B"} {
        close $storage_report
        error "Tied DSP clock does not use direct combinational multiplication: $owner"
    }
    set opmode 0
    foreach pattern {CLK OPMODE* ALUMODE* INMODE* CARRYINSEL* CARRYIN CEC CED CEAD} {
        foreach control [get_pins -quiet $owner/$pattern] {
            set nets [get_nets -quiet -segments -of_objects $control]
            set drivers [get_pins -quiet -leaf -of_objects $nets -filter {DIRECTION == OUT}]
            set cells [get_cells -quiet -of_objects $drivers]
            set ref [get_property REF_NAME $cells]
            set name [get_property REF_PIN_NAME $control]
            if {[llength $cells] != 1 || $ref ni {GND VCC} ||
                (![string match "OPMODE*" $name] && $ref ne "GND")} {
                close $storage_report
                error "Unexpected tied-DSP control: $control ($cells)"
            }
            if {[regexp {^OPMODE\[([0-8])\]$} $name -> bit] && $ref eq "VCC"} {
                set opmode [expr {$opmode | (1 << $bit)}]
            }
        }
    }
    # Multiplier/cascade modes do not use the default-enabled C/D/AD registers.
    if {$opmode ni {5 85}} {
        close $storage_report
        error "Unexpected combinational DSP operation mode: $owner $opmode"
    }
    puts $storage_report "COMBINATIONAL_DSP_TIED_CLOCK $owner OPMODE $opmode"
}
puts $storage_report "COMBINATIONAL_DSP_COUNT [dict size $checked_dsps]"
set active_pins [filter $clock_pins {IS_TIED == 0}]
if {[llength $active_pins] == 0} {
    close $storage_report
    error "No active mapped NE16 clock pins found"
}
set clock_nets [get_nets -quiet -of_objects $active_pins]
foreach net $clock_nets {
    set segments [get_nets -quiet -segments $net]
    set drivers [get_pins -quiet -leaf -of_objects $segments -filter {DIRECTION == OUT}]
    set cells [get_cells -quiet -of_objects $drivers]
    puts $storage_report "NE16_CLOCK_NET $net DRIVER_CELLS $cells"
    if {[llength $cells] != 1 ||
        ![string match "BUFG*" [get_property REF_NAME $cells]]} {
        close $storage_report
        error "NE16 clock is not driven by a global clock buffer: $net ($cells)"
    }
}
close $storage_report
puts "MAPPED_NE16_STORAGE_REVIEW LATCHES 0 CLOCK_PINS [llength $clock_pins]"
check_timing -verbose -file ${report_dir}/check_timing.rpt
report_methodology -file ${report_dir}/methodology.rpt
report_clocks -file ${report_dir}/clocks.rpt
report_clock_interaction -file ${report_dir}/clock_interaction.rpt
report_exceptions -file ${report_dir}/exceptions.rpt
report_bus_skew -file ${report_dir}/bus_skew.rpt
report_timing_summary -report_unconstrained -file ${report_dir}/timing_summary.rpt
report_cdc -details -file ${report_dir}/cdc.rpt
set violations [get_cdc_violations]
set cdc_report [open ${report_dir}/cdc_objects.rpt w]
puts $cdc_report "CDC_VIOLATION_COUNT [llength $violations]"
foreach violation $violations {
    puts $cdc_report "CDC_VIOLATION $violation"
    puts $cdc_report [report_property -return_string $violation]
}
close $cdc_report
puts "FRESH_IMPLEMENTATION_REVIEW_REPORTS $report_dir"
close_project
exit 0
