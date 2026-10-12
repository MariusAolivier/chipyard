if {[llength $argv] != 1 || ![info exists ::env(SLURM_JOB_ID)]} {
    error "Run through Slurm with -tclargs <NE16-cl-directory>"
}
set root_dir [file normalize [lindex $argv 0]]
source ${root_dir}/scripts/platform_env_2024.2.tcl
open_project ${root_dir}/vivado_proj/firesim.xpr
if {[get_property TOP [current_fileset]] ne "overall_fpga_top" ||
    [get_property BOARD_PART [current_project]] ne "xilinx.com:au250:part0:1.3" ||
    [get_property STATUS [get_runs impl_1]] ne "write_bitstream Complete!"} {
    error "Expected a completed U250 FireSim implementation"
}
open_run impl_1

set report_dir ${root_dir}/vivado_proj/constraint_audit_$::env(SLURM_JOB_ID)
if {[file exists $report_dir]} {
    error "Constraint audit already exists: $report_dir"
}
file mkdir $report_dir
set report [open ${report_dir}/clock_reset_connectivity.rpt w]
foreach pattern {host_clock mmcm_clkout0 pcie_refclk_clk_p *bufg_gt_intclk/O} {
    set clock [get_clocks -quiet $pattern]
    if {[llength $clock] != 1} {
        error "Expected exactly one clock matching $pattern; found: $clock"
    }
    puts $report [report_property -return_string $clock]
    if {$pattern eq "pcie_refclk_clk_p" || $pattern eq "*bufg_gt_intclk/O"} {
        set registers [all_registers -clock $clock]
        set outside 0
        foreach register $registers {
            set name [get_property NAME $register]
            if {![string match "design_1_i/xdma_0/*" $name]} {
                incr outside
                puts $report "OUTSIDE_XDMA_REGISTER $clock $name"
            }
        }
        puts $report "CLOCK_REGISTER_SCOPE $clock TOTAL [llength $registers] OUTSIDE_XDMA $outside"
        puts "CLOCK_REGISTER_SCOPE $clock TOTAL [llength $registers] OUTSIDE_XDMA $outside"
    }
}

foreach name {resetn pcie_perstn ddr4_sdram_c0_reset_n} {
    set port [get_ports -quiet $name]
    if {[llength $port] != 1} {
        error "Expected exactly one reset port: $name"
    }
    if {[get_property DIRECTION $port] eq "IN"} {
        set endpoints [all_fanout -flat -endpoints_only -from $port]
    } else {
        set endpoints [all_fanin -flat -startpoints_only -to $port]
    }
    puts $report "RESET_PORT $name DIRECTION [get_property DIRECTION $port] ENDPOINTS [llength $endpoints]"
    puts "RESET_PORT $name ENDPOINTS [llength $endpoints]"
    foreach endpoint $endpoints {
        set cell [get_cells -quiet -of_objects $endpoint]
        if {[llength $cell] == 0} {
            puts $report "RESET_ENDPOINT $endpoint PORT"
        } else {
            set ref [get_property REF_NAME $cell]
            set line "RESET_ENDPOINT $endpoint CELL $cell TYPE $ref"
            if {[string match "FD*" $ref] || [string match "LD*" $ref]} {
                append line " ASYNC_REG [get_property ASYNC_REG $cell]"
            }
            puts $report $line
        }
    }
}
close $report
report_bus_skew -file ${report_dir}/bus_skew.rpt
set skew_report [open ${report_dir}/bus_skew.rpt r]
set in_summary 0
set constraints 0
set worst_slack {}
while {[gets $skew_report line] >= 0} {
    if {$line eq "1. Bus Skew Report Summary"} {
        set in_summary 1
    } elseif {$line eq "2. Bus Skew Report Per Constraint"} {
        set in_summary 0
    } elseif {$in_summary && [regexp {^\s+(Slow|Fast)\s} $line]} {
        if {![regexp {^\s+(?:Slow|Fast)\s+[+-]?[0-9]+(?:\.[0-9]+)?\s+[+-]?[0-9]+(?:\.[0-9]+)?\s+([+-]?[0-9]+(?:\.[0-9]+)?)\s*$} $line -> slack]} {
            error "Unexpected bus-skew summary row: $line"
        }
        incr constraints
        if {$worst_slack eq {} || $slack < $worst_slack} {
            set worst_slack $slack
        }
    }
}
close $skew_report
if {$constraints == 0 || $worst_slack < 0} {
    error "Bus-skew audit failed: $constraints constraints, worst slack $worst_slack"
}
puts "BUS_SKEW_AUDIT CONSTRAINTS $constraints WORST_SLACK_NS $worst_slack"
puts "CONSTRAINT_CONNECTIVITY_REPORT ${report_dir}/clock_reset_connectivity.rpt"
close_project
exit 0
