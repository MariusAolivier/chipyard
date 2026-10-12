if {[llength $argv] != 1 || ![info exists ::env(SLURM_JOB_ID)]} {
    error "Run through an exclusive FPGA Slurm allocation with -tclargs <target-UID>"
}
set serial [lindex $argv 0]
if {$serial eq {}} {
    error "An explicit target UID is required"
}
set_param labtools.enable_cs_server false
open_hw_manager
connect_hw_server -url 127.0.0.1:3121 -allow_non_jtag
close_hw_target
puts "HW_TARGETS [get_hw_targets]"
set targets {}
foreach target [get_hw_targets] {
    if {[string first $serial $target] != -1} {
        lappend targets $target
    }
}
if {[llength $targets] != 1} {
    error "Expected exactly one selected-card JTAG target; found: $targets"
}
open_hw_target [lindex $targets 0]
set devices [get_hw_devices]
if {[llength $devices] != 1 ||
    ![string match "xcu250*" [get_property PART $devices]]} {
    error "Expected exactly one U250 device on the selected target; found: $devices"
}
puts "SELECTED_U250_TARGET_PRESENT [lindex $targets 0] DEVICE $devices"
close_hw_target
close_hw_manager
exit 0
