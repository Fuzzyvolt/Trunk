# Project 04: AXI DMA loopback.
# Tested with Vivado 2026.1 (Basic): bitstream loads, 100 x 64 KiB loopbacks pass, 393 MB/s at 1 MiB.
#
# Run on the laptop (after "vivado-env"):
#   cd ~/microzed-linux/microzed/fpga/04-dma-loopback && vivado -mode batch -source build.tcl
#
# The Vivado project goes OUTSIDE the git repo (default ~/vivado/mz04_dma).
# Outputs in that folder:
#   mz04_dma.bit   bitstream
#   mz04_dma.bin   byte-swapped bitstream for the Linux FPGA manager
#   mz04_dma.xsa   hardware description (not needed on the board)
#
# Only one bitstream can be loaded at a time, so this is the project 03
# design (axiregs + pltimer, same addresses and interrupt) plus the DMA loop.
# The axiregs ID is changed to 0x4D5A0401, so S40pl can tell which design is
# loaded and only loads the DMA drivers for this one.
#
#                M_AXI_GP0                         (CPU writes DMA registers)
#   CPU (PS) ---------------> axi_gp0_ic --+--> axiregs_0      0x43C00000  4K
#                                          +--> pltimer_0      0x43C10000  4K
#                                          +--> axi_dma_0 lite 0x40400000 64K
#
#   DDR <--- S_AXI_HP0 <--- axi_hp0_ic <---+--- M_AXI_MM2S  (DMA reads DDR)
#                                          +--- M_AXI_S2MM  (DMA writes DDR)
#
#   axi_dma_0 M_AXIS_MM2S --> axis_fifo (4 KB) --> S_AXIS_S2MM axi_dma_0
#
#   pltimer irq  --> irq_concat In0 --> IRQ_F2P[0] = GIC ID 61 (DT: <0 29 4>, as in project 03)
#   mm2s_introut --> irq_concat In1 --> IRQ_F2P[1] = GIC ID 62 (DT: <0 30 4>)
#   s2mm_introut --> irq_concat In2 --> IRQ_F2P[2] = GIC ID 63 (DT: <0 31 4>)
#
#   FCLK_CLK0 (100 MHz) clocks all of the PL; rst_pl makes the resets.
#
# Everything is wired by hand (no connection automation), so each wire in the
# picture above is one line below, and the names are fixed for the checks.

set name    mz04_dma
set part    xc7z010clg400-1
set here    [file dirname [file normalize [info script]]]
set projdir [file normalize ~/vivado/$name]
if {[llength $argv] > 0} { set projdir [file normalize [lindex $argv 0]] }

set regs_addr 0x43C00000
set timer_addr 0x43C10000
set dma_addr  0x40400000
set design_id 0x4D5A0401
# AXI DMA "Width of Buffer Length Register": max bytes per transfer = 2^n - 1.
# Default 14 (16 KB). 23 = 8 MB, the Linux driver's default; the device tree
# property xlnx,sg-length-width must say the same.
set len_width 23

source $here/../02-axiregs/bit2bin.tcl

# Latest version of a Xilinx IP, so the script does not break on a new Vivado.
proc ipdef {name} {
    set v [lindex [lsort -dictionary [get_ipdefs -all xilinx.com:ip:${name}:*]] end]
    if {$v eq ""} { error "IP $name not found in this Vivado" }
    return $v
}

create_project $name $projdir -part $part -force

set bp [lindex [lsort -dictionary [get_board_parts -quiet *microzed_7010*]] end]
if {$bp eq ""} {
    error "MicroZed 7010 board file not found. Install the Avnet bdf repo and add it to board.repoPaths (see ../README.md)."
}
puts "Using board part: $bp"
set_property board_part $bp [current_project]

# Projects 02 and 03 HDL, used from their folders (not copied).
add_files -norecurse [list $here/../02-axiregs/axiregs.v $here/../03-pltimer/pltimer.v]
update_compile_order -fileset sources_1

create_bd_design system

# ---------------- PS ----------------
create_bd_cell -type ip -vlnv [ipdef processing_system7] ps7
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
    -config {make_external "FIXED_IO, DDR" apply_board_preset "1" Master "Disable" Slave "Disable"} \
    [get_bd_cells ps7]

# GP0 master (CPU -> PL), HP0 slave (PL -> DDR), interrupts from the PL.
# HP0 stays 64 bits wide (the default): U-Boot's own ps7_init does not set up
# the HP ports, and 64 bits is their reset value, so nothing else is needed.
set_property -dict [list \
    CONFIG.PCW_USE_M_AXI_GP0 {1} \
    CONFIG.PCW_USE_S_AXI_HP0 {1} \
    CONFIG.PCW_S_AXI_HP0_DATA_WIDTH {64} \
    CONFIG.PCW_USE_FABRIC_INTERRUPT {1} \
    CONFIG.PCW_IRQ_F2P_INTR {1} \
    CONFIG.PCW_EN_CLK0_PORT {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
] [get_bd_cells ps7]

# ---------------- Blocks ----------------
create_bd_cell -type ip -vlnv [ipdef proc_sys_reset] rst_pl

create_bd_cell -type module -reference axiregs axiregs_0
set_property CONFIG.ID $design_id [get_bd_cells axiregs_0]
create_bd_cell -type module -reference pltimer pltimer_0

# AXI DMA in simple (direct register) mode: no scatter-gather, one buffer per
# transfer. 32-bit memory and stream sides; 16-beat bursts (the AXI3 maximum
# on the HP port).
create_bd_cell -type ip -vlnv [ipdef axi_dma] axi_dma_0
set_property -dict [list \
    CONFIG.c_include_sg {0} \
    CONFIG.c_sg_length_width $len_width \
    CONFIG.c_addr_width {32} \
    CONFIG.c_include_mm2s {1} \
    CONFIG.c_include_s2mm {1} \
    CONFIG.c_m_axi_mm2s_data_width {32} \
    CONFIG.c_m_axis_mm2s_tdata_width {32} \
    CONFIG.c_m_axi_s2mm_data_width {32} \
    CONFIG.c_s_axis_s2mm_tdata_width {32} \
    CONFIG.c_mm2s_burst_size {16} \
    CONFIG.c_s2mm_burst_size {16} \
] [get_bd_cells axi_dma_0]

# The loop: a FIFO between the DMA's stream output and its stream input.
# 1024 words x 4 bytes = 4 KB. Width and TLAST come from the DMA's stream port.
create_bd_cell -type ip -vlnv [ipdef axis_data_fifo] axis_fifo
set_property CONFIG.FIFO_DEPTH {1024} [get_bd_cells axis_fifo]

# Three interrupt wires -> one 3-bit IRQ_F2P bus. Newer Vivado versions replace
# xlconcat with the inline "ilconcat"; use whichever exists.
set concat [lindex [lsort -dictionary [concat \
    [get_ipdefs -all xilinx.com:ip:xlconcat:*] \
    [get_ipdefs -all xilinx.com:inline_hdl:ilconcat:*]]] end]
if {$concat eq ""} { error "No concat IP (xlconcat/ilconcat) found" }
create_bd_cell -type ip -vlnv $concat irq_concat
set_property CONFIG.NUM_PORTS {3} [get_bd_cells irq_concat]

# CPU side: 1 master in (GP0), 3 slaves out. Converts AXI3 -> AXI4-Lite.
create_bd_cell -type ip -vlnv [ipdef axi_interconnect] axi_gp0_ic
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {3}] [get_bd_cells axi_gp0_ic]

# Memory side: 2 masters in (MM2S, S2MM), 1 out (HP0). Converts 32 -> 64 bits.
create_bd_cell -type ip -vlnv [ipdef axi_interconnect] axi_hp0_ic
set_property -dict [list CONFIG.NUM_SI {2} CONFIG.NUM_MI {1}] [get_bd_cells axi_hp0_ic]

# ---------------- AXI connections ----------------
connect_bd_intf_net [get_bd_intf_pins ps7/M_AXI_GP0]        [get_bd_intf_pins axi_gp0_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins axi_gp0_ic/M00_AXI]   [get_bd_intf_pins axiregs_0/s_axi]
connect_bd_intf_net [get_bd_intf_pins axi_gp0_ic/M01_AXI]   [get_bd_intf_pins pltimer_0/s_axi]
connect_bd_intf_net [get_bd_intf_pins axi_gp0_ic/M02_AXI]   [get_bd_intf_pins axi_dma_0/S_AXI_LITE]

connect_bd_intf_net [get_bd_intf_pins axi_dma_0/M_AXI_MM2S] [get_bd_intf_pins axi_hp0_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins axi_dma_0/M_AXI_S2MM] [get_bd_intf_pins axi_hp0_ic/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins axi_hp0_ic/M00_AXI]   [get_bd_intf_pins ps7/S_AXI_HP0]

connect_bd_intf_net [get_bd_intf_pins axi_dma_0/M_AXIS_MM2S] [get_bd_intf_pins axis_fifo/S_AXIS]
connect_bd_intf_net [get_bd_intf_pins axis_fifo/M_AXIS]      [get_bd_intf_pins axi_dma_0/S_AXIS_S2MM]

# ---------------- Clock: FCLK_CLK0 to every clock pin ----------------
connect_bd_net [get_bd_pins ps7/FCLK_CLK0] \
    [get_bd_pins ps7/M_AXI_GP0_ACLK] \
    [get_bd_pins ps7/S_AXI_HP0_ACLK] \
    [get_bd_pins rst_pl/slowest_sync_clk] \
    [get_bd_pins axiregs_0/s_axi_aclk] \
    [get_bd_pins pltimer_0/s_axi_aclk] \
    [get_bd_pins axi_dma_0/s_axi_lite_aclk] \
    [get_bd_pins axi_dma_0/m_axi_mm2s_aclk] \
    [get_bd_pins axi_dma_0/m_axi_s2mm_aclk] \
    [get_bd_pins axis_fifo/s_axis_aclk] \
    [get_bd_pins axi_gp0_ic/ACLK] \
    [get_bd_pins axi_gp0_ic/S00_ACLK] \
    [get_bd_pins axi_gp0_ic/M00_ACLK] \
    [get_bd_pins axi_gp0_ic/M01_ACLK] \
    [get_bd_pins axi_gp0_ic/M02_ACLK] \
    [get_bd_pins axi_hp0_ic/ACLK] \
    [get_bd_pins axi_hp0_ic/S00_ACLK] \
    [get_bd_pins axi_hp0_ic/S01_ACLK] \
    [get_bd_pins axi_hp0_ic/M00_ACLK]

# ---------------- Resets ----------------
# The PS reset output goes through rst_pl, which syncs it to the clock.
connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins rst_pl/ext_reset_in]
connect_bd_net [get_bd_pins rst_pl/interconnect_aresetn] \
    [get_bd_pins axi_gp0_ic/ARESETN] \
    [get_bd_pins axi_hp0_ic/ARESETN]
connect_bd_net [get_bd_pins rst_pl/peripheral_aresetn] \
    [get_bd_pins axiregs_0/s_axi_aresetn] \
    [get_bd_pins pltimer_0/s_axi_aresetn] \
    [get_bd_pins axi_dma_0/axi_resetn] \
    [get_bd_pins axis_fifo/s_axis_aresetn] \
    [get_bd_pins axi_gp0_ic/S00_ARESETN] \
    [get_bd_pins axi_gp0_ic/M00_ARESETN] \
    [get_bd_pins axi_gp0_ic/M01_ARESETN] \
    [get_bd_pins axi_gp0_ic/M02_ARESETN] \
    [get_bd_pins axi_hp0_ic/S00_ARESETN] \
    [get_bd_pins axi_hp0_ic/S01_ARESETN] \
    [get_bd_pins axi_hp0_ic/M00_ARESETN]

# ---------------- Interrupts ----------------
connect_bd_net [get_bd_pins pltimer_0/irq]          [get_bd_pins irq_concat/In0]
connect_bd_net [get_bd_pins axi_dma_0/mm2s_introut] [get_bd_pins irq_concat/In1]
connect_bd_net [get_bd_pins axi_dma_0/s2mm_introut] [get_bd_pins irq_concat/In2]
connect_bd_net [get_bd_pins irq_concat/dout]        [get_bd_pins ps7/IRQ_F2P]

# ---------------- Addresses ----------------
assign_bd_address

proc set_seg {pattern offset range} {
    set seg [get_bd_addr_segs -quiet $pattern]
    if {[llength $seg] != 1} {
        error "Address segment $pattern not found. Have: [get_bd_addr_segs]"
    }
    set_property range  $range  $seg
    set_property offset $offset $seg
    puts "$seg: offset [get_property offset $seg], range [get_property range $seg]"
}
# What the CPU sees (the device tree "reg" values must match).
set_seg ps7/Data/SEG_axiregs_0_*  $regs_addr 4K
set_seg ps7/Data/SEG_pltimer_0_*  $timer_addr 4K
set_seg ps7/Data/SEG_axi_dma_0_*  $dma_addr  64K
# What the DMA sees: all 1 GB of DDR. A smaller window would make the
# interconnect reject (DECERR) buffers Linux places above it.
set_seg axi_dma_0/Data_MM2S/SEG_ps7_HP0_DDR_LOWOCM 0x00000000 1G
set_seg axi_dma_0/Data_S2MM/SEG_ps7_HP0_DDR_LOWOCM 0x00000000 1G

validate_bd_design

# Checks on values Vivado fills in during validation.
set tlast [get_property CONFIG.HAS_TLAST [get_bd_cells axis_fifo]]
if {$tlast != 1} {
    error "axis_fifo has no TLAST: S2MM would never see the end of a packet"
}
puts "axis_fifo: [get_property CONFIG.TDATA_NUM_BYTES [get_bd_cells axis_fifo]] bytes wide, TLAST $tlast"
puts "axiregs_0 ID: [get_property CONFIG.ID [get_bd_cells axiregs_0]]"
puts "axi_dma_0 length width: [get_property CONFIG.c_sg_length_width [get_bd_cells axi_dma_0]] bits"

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

open_run impl_1
set wns [get_property SLACK [get_timing_paths -max_paths 1 -setup]]
puts "Worst negative slack (setup): $wns ns (must be >= 0)"
report_utilization -file $projdir/utilization.txt

set bit [glob $projdir/$name.runs/impl_1/*.bit]
file copy -force $bit $projdir/$name.bit
bit2bin $projdir/$name.bit $projdir/$name.bin
write_hw_platform -fixed -include_bit -force $projdir/$name.xsa

puts "DONE: $projdir/$name.bit, $projdir/$name.bin, $projdir/$name.xsa"
