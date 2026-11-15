# Project 02: Zynq PS + our AXI4-Lite register block (axiregs.v) on M_AXI_GP0.
# Not yet tested in Vivado (written for 2026.1, same flow as 01-ps-only/build.tcl).
#
# Run on the laptop (after "vivado-env"):
#   cd ~/microzed-linux/microzed/fpga/02-axiregs && vivado -mode batch -source build.tcl
#
# The Vivado project goes OUTSIDE the git repo (default ~/vivado/mz02_axiregs).
# Outputs in that folder:
#   mz02_axiregs.bit   bitstream (for JTAG or U-Boot)
#   axiregs.bin        byte-swapped bitstream for the Linux FPGA manager
#   mz02_axiregs.xsa   hardware description (not needed on the board)
#
#   CPU (PS) --M_AXI_GP0 (AXI3)--> AXI Interconnect --(AXI4-Lite)--> axiregs
#                                  (protocol converter)              0x43C00000
#   FCLK_CLK0 (100 MHz) clocks all of it; proc_sys_reset makes the reset.

set name    mz02_axiregs
set part    xc7z010clg400-1
set here    [file dirname [file normalize [info script]]]
set projdir [file normalize ~/vivado/$name]
if {[llength $argv] > 0} { set projdir [file normalize [lindex $argv 0]] }

set base_addr 0x43C00000

source $here/bit2bin.tcl

create_project $name $projdir -part $part -force

set bp [lindex [lsort -dictionary [get_board_parts -quiet *microzed_7010*]] end]
if {$bp eq ""} {
    error "MicroZed 7010 board file not found. Install the Avnet bdf repo and add it to board.repoPaths (see ../README.md)."
}
puts "Using board part: $bp"
set_property board_part $bp [current_project]

# Our HDL. Added by reference (not copied), so edits in the repo are used.
add_files -norecurse $here/axiregs.v
update_compile_order -fileset sources_1

# Block design: PS with the board preset, as in project 01.
create_bd_design system
set ps7_vlnv [lindex [lsort -dictionary [get_ipdefs -all xilinx.com:ip:processing_system7:*]] end]
create_bd_cell -type ip -vlnv $ps7_vlnv ps7
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
    -config {make_external "FIXED_IO, DDR" apply_board_preset "1" Master "Disable" Slave "Disable"} \
    [get_bd_cells ps7]

# Turn on the GP0 master port (the CPU's window into the PL) and the PL clock.
# Note: this FCLK setting only sets the timing constraint here. On the board,
# U-Boot's SPL sets the real clock (it uses its own ps7_init, also 100 MHz).
set_property -dict [list \
    CONFIG.PCW_USE_M_AXI_GP0 {1} \
    CONFIG.PCW_EN_CLK0_PORT {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
] [get_bd_cells ps7]

# Our module as a block ("module reference": plain HDL, no IP packaging).
create_bd_cell -type module -reference axiregs axiregs_0

# Same as "Run Connection Automation" in the GUI: adds an AXI Interconnect
# (converts GP0's AXI3 to AXI4-Lite) and a Processor System Reset, and wires
# FCLK_CLK0 and the resets.
apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config [list \
    Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} \
    Master {/ps7/M_AXI_GP0} Slave {/axiregs_0/s_axi} \
    ddr_seg {Auto} intc_ip {New AXI Interconnect} master_apm {0} \
] [get_bd_intf_pins axiregs_0/s_axi]

# Address: 4 KB at 0x43C00000 (the device tree node uses the same numbers).
assign_bd_address
set seg [get_bd_addr_segs -quiet ps7/Data/SEG_axiregs_0_*]
if {[llength $seg] != 1} {
    error "Address segment for axiregs_0 not found. Check the Address Editor. Found: [get_bd_addr_segs ps7/Data/*]"
}
set_property range 4K $seg
set_property offset $base_addr $seg
puts "axiregs_0: offset [get_property offset $seg], range [get_property range $seg]"

validate_bd_design
save_bd_design

set bd_file [get_files system.bd]
generate_target all $bd_file
make_wrapper -files $bd_file -top -import
set_property top system_wrapper [current_fileset]
update_compile_order -fileset sources_1

launch_runs impl_1 -to_step write_bitstream -jobs 2
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
    error "Implementation or bitstream failed. See $projdir/$name.runs/impl_1/runme.log"
}

# Timing: WNS < 0 means the logic is too slow for 100 MHz.
open_run impl_1
set wns [get_property SLACK [get_timing_paths -max_paths 1 -setup]]
puts "Worst negative slack (setup): $wns ns (must be >= 0)"

set bit [glob $projdir/$name.runs/impl_1/*.bit]
file copy -force $bit $projdir/$name.bit
bit2bin $projdir/$name.bit $projdir/axiregs.bin
write_hw_platform -fixed -include_bit -force $projdir/$name.xsa

puts "DONE: $projdir/$name.bit, $projdir/axiregs.bin, $projdir/$name.xsa"
