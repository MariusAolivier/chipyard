set jobs 8
if {[info exists ::env(SLURM_CPUS_PER_TASK)]} {
    if {![string is integer -strict $::env(SLURM_CPUS_PER_TASK)] ||
        $::env(SLURM_CPUS_PER_TASK) < 1} {
        error "SLURM_CPUS_PER_TASK must be a positive integer"
    }
    set jobs $::env(SLURM_CPUS_PER_TASK)
}
set_param general.maxThreads [expr {min(8, $jobs)}]

if {[version -short] ne "2024.2"} {
    error "This U250 platform overlay requires Vivado 2024.2"
}
if {![info exists ::env(NE16_U250_BOARD_REPO)]} {
    error "Set NE16_U250_BOARD_REPO to the installed U250 board repository"
}

set board_repository [file normalize $::env(NE16_U250_BOARD_REPO)]
if {![file isdirectory $board_repository]} {
    error "Board repository does not exist: $board_repository"
}

set board_repositories [get_param board.repoPaths]
lappend board_repositories $board_repository
set_param board.repoPaths $board_repositories

set installed_board_parts [get_board_parts -quiet xilinx.com:au250:part0:1.3]
if {[llength $installed_board_parts] != 1} {
    error "Expected exactly one U250 1.3 board definition; found: $installed_board_parts"
}
puts "NE16_U250_BOARD_REPOSITORY $board_repository"
