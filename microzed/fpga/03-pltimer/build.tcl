# Project 03: Zynq PS + axiregs (project 02) + pltimer (timer with interrupt).
# Tested with Vivado 2026.1 (Basic): bitstream loads, interrupts reach Linux.
#
# Run on the laptop (after "vivado-env"):
#   cd ~/microzed-linux/microzed/fpga/03-pltimer && vivado -mode batch -source build.tcl
#
# The Vivado project goes OUTSIDE the git repo (default ~/vivado/mz03_pltimer).
# Outputs in that folder:
#   mz03_pltimer.bit   bitstream
#   mz03_pltimer.bin   byte-swapped bitstream for the Linux FPGA manager
#   mz03_pltimer.xsa   hardware description (not needed on the board)
#
#                                                  +--> axiregs_0  0x43C00000 (project 02, unchanged)
#   CPU (PS) --M_AXI_GP0--> AXI Interconnect ------+
#                                                  +--> pltimer_0  0x43C10000
#                                                          |
#   GIC <-- IRQ_F2P[0] (GIC ID 61, DT "0 29 4") <------ irq
#
# Only one bitstream can be loaded at a time, so axiregs stays in this design:
# the project 02 driver and the web page keep working.

set name    mz03_pltimer
set part    xc7z010clg400-1
set here    [file dirname [file normalize [info script]]]
set projdir [file normalize ~/vivado/$name]
if {[llength $argv] > 0} { set projdir [file normalize [lindex $argv 0]] }

set axiregs_addr 0x43C00000
set pltimer_addr 0x43C10000

source $here/../02-axiregs/bit2bin.tcl

create_project $name $projdir -part $part -force

set bp [lindex [lsort -dictionary [get_board_parts -quiet *microzed_7010*]] end]
if {$bp eq ""} {
    error "MicroZed 7010 board file not found. Install the Avnet bdf repo and add it to board.repoPaths (see ../README.md)."
}
puts "Using board part: $bp"
set_property board_part $bp [current_project]

# Our HDL, added by reference (not copied): edits in the repo are used.
add_files -norecurse [list $here/../02-axiregs/axiregs.v $here/pltimer.v]
update_compile_order -fileset sources_1

# Block design: PS with the board preset.
create_bd_design system
set ps7_vlnv [lindex [lsort -dictionary [get_ipdefs -all xilinx.com:ip:processing_system7:*]] end]
create_bd_cell -type ip -vlnv $ps7_vlnv ps7
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
    -config {make_external "FIXED_IO, DDR" apply_board_preset "1" Master "Disable" Slave "Disable"} \
    [get_bd_cells ps7]

# GP0 master port + PL clock (as in project 02), and now also the
# PL-to-PS interrupt input IRQ_F2P ("fabric interrupts" in the GUI:
# Interrupts -> Fabric Interrupts -> PL-PS Interrupt Ports -> IRQ_F2P).
set_property -dict [list \
    CONFIG.PCW_USE_M_AXI_GP0 {1} \
    CONFIG.PCW_EN_CLK0_PORT {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
    CONFIG.PCW_USE_FABRIC_INTERRUPT {1} \
    CONFIG.PCW_IRQ_F2P_INTR {1} \
] [get_bd_cells ps7]

# Our two modules as blocks ("module reference": plain HDL, no IP packaging).
create_bd_cell -type module -reference axiregs axiregs_0
create_bd_cell -type module -reference pltimer pltimer_0

# "Run Connection Automation": the first call adds the AXI Interconnect and the
# Processor System Reset; the second adds a second port to that interconnect.
foreach slave {axiregs_0 pltimer_0} {
    set ic [get_bd_cells -quiet ps7_axi_periph]
    set intc [expr {$ic eq "" ? "New AXI Interconnect" : "/ps7_axi_periph"}]
    apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config [list \
        Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} \
        Master {/ps7/M_AXI_GP0} Slave /$slave/s_axi \
        ddr_seg {Auto} intc_ip $intc master_apm {0} \
    ] [get_bd_intf_pins $slave/s_axi]
}

# The interrupt wire. With one source it connects straight to IRQ_F2P, which
# becomes IRQ_F2P[0:0]. With several, put an xlconcat block in between:
# its input 0 -> IRQ_F2P[0] (GIC ID 61), input 1 -> IRQ_F2P[1] (ID 62), ...
connect_bd_net [get_bd_pins pltimer_0/irq] [get_bd_pins ps7/IRQ_F2P]

# Addresses: 4 KB each (the device tree nodes use the same numbers).
assign_bd_address
foreach {cell addr} [list axiregs_0 $axiregs_addr pltimer_0 $pltimer_addr] {
    set seg [get_bd_addr_segs -quiet ps7/Data/SEG_${cell}_*]
    if {[llength $seg] != 1} {
        error "Address segment for $cell not found. Check the Address Editor. Found: [get_bd_addr_segs ps7/Data/*]"
    }
    set_property range 4K $seg
    set_property offset $addr $seg
    puts "$cell: offset [get_property offset $seg], range [get_property range $seg]"
}

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
bit2bin $projdir/$name.bit $projdir/$name.bin
write_hw_platform -fixed -include_bit -force $projdir/$name.xsa

puts "DONE: $projdir/$name.bit, $projdir/$name.bin, $projdir/$name.xsa"
