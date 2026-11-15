# Project 01: a block design with only the Zynq PS, built to a bitstream.
#
# Run on the laptop (after "source <Vivado>/settings64.sh"):
#   vivado -mode batch -source build.tcl
#   vivado -mode batch -source build.tcl -tclargs /some/other/dir
#
# The Vivado project goes OUTSIDE the git repo (default ~/vivado/mz01_ps_only).
# Outputs: <dir>/mz01_ps_only.bit and <dir>/mz01_ps_only.xsa

set name    mz01_ps_only
set part    xc7z010clg400-1
set projdir [file normalize ~/vivado/$name]
if {[llength $argv] > 0} { set projdir [file normalize [lindex $argv 0]] }

create_project $name $projdir -part $part -force

# Avnet board file: gives the PS its MicroZed DDR3, MIO and clock presets.
set bp [lindex [lsort -dictionary [get_board_parts -quiet *microzed_7010*]] end]
if {$bp eq ""} {
    error "MicroZed 7010 board file not found. Install the Avnet bdf repo and add it to board.repoPaths (see README.md)."
}
puts "Using board part: $bp"
set_property board_part $bp [current_project]

# Block design with one IP: the Zynq7 Processing System.
create_bd_design system
set ps7_vlnv [lindex [lsort -dictionary [get_ipdefs -all xilinx.com:ip:processing_system7:*]] end]
create_bd_cell -type ip -vlnv $ps7_vlnv ps7

# Same as "Run Block Automation" in the GUI with "Apply Board Preset" ticked:
# DDR and FIXED_IO (MIO pins) become external ports.
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
    -config {make_external "FIXED_IO, DDR" apply_board_preset "1" Master "Disable" Slave "Disable"} \
    [get_bd_cells ps7]

# If the preset enables the M_AXI_GP0 port, its clock input must be driven,
# or validation fails. Feed it from the PS's own PL clock FCLK_CLK0.
set gp0_clk [get_bd_pins -quiet ps7/M_AXI_GP0_ACLK]
set fclk0   [get_bd_pins -quiet ps7/FCLK_CLK0]
if {[llength $gp0_clk] && [llength $fclk0]} {
    connect_bd_net $fclk0 $gp0_clk
}

validate_bd_design
save_bd_design

# HDL wrapper (top level) around the block design.
set bd_file [get_files system.bd]
generate_target all $bd_file
make_wrapper -files $bd_file -top -import
set_property top system_wrapper [current_fileset]
update_compile_order -fileset sources_1

# Synthesis, implementation, bitstream.
launch_runs impl_1 -to_step write_bitstream -jobs 2
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
    error "Implementation or bitstream failed. See $projdir/$name.runs/impl_1/runme.log"
}

set bit [glob $projdir/$name.runs/impl_1/*.bit]
file copy -force $bit $projdir/$name.bit
write_hw_platform -fixed -include_bit -force $projdir/$name.xsa

# Print the PS settings that matter for project 02.
set ps [get_bd_cells ps7]
foreach p {PCW_FPGA0_PERIPHERAL_FREQMHZ PCW_USE_M_AXI_GP0 PCW_USE_FABRIC_INTERRUPT PCW_UIPARAM_DDR_PARTNO} {
    puts [format "%-32s %s" $p [get_property CONFIG.$p $ps]]
}
puts "DONE: $projdir/$name.bit and $projdir/$name.xsa"
