# FPGA (PL) sources

Only small sources live here (Tcl, HDL, constraints). The Vivado projects themselves are generated
outside the repo, in `~/vivado/<name>`, because they are large and full of generated files.

| Folder | What it is |
|---|---|
| `01-ps-only/build.tcl` | Project 01: block design with only the Zynq PS, builds `.bit` and `.xsa` (tested, works with Vivado 2026.1) |
| `02-axiregs/axiregs.v` | Project 02: AXI4-Lite slave with 5 registers (reg0, reg1, sum, count, id) |
| `02-axiregs/tb_axiregs.v` | Testbench for it (Icarus Verilog or Vivado simulator) |
| `02-axiregs/build.tcl` | Project 02: PS + axiregs on M_AXI_GP0 at `0x43C00000`, builds `.bit`, `axiregs.bin`, `.xsa` (tested, works on the board) |
| `02-axiregs/bit2bin.tcl` | Converts `.bit` to the byte-swapped `.bin` the Linux FPGA manager loads (replaces `bootgen`) |
| `04-dma-loopback/build.tcl` | Project 04: axiregs (ID `0x4D5A0401`) + AXI DMA at `0x40400000` with an AXI4-Stream FIFO loop, HP0 to DDR, interrupts on IRQ_F2P. Builds `mz04_dma.bin` (not yet tested) |

## One-time setup (laptop)

Load Vivado into the shell (path depends on the version installed):

```bash
source /tools/Xilinx/2026.1/Vivado/settings64.sh
```

Avnet board files (so `microzed_7010` exists as a board):

```bash
git clone https://github.com/Avnet/bdf.git ~/vivado-boards/avnet-bdf
```

```bash
mkdir -p ~/.Xilinx/Vivado && echo 'set_param board.repoPaths [list /home/fabio/vivado-boards/avnet-bdf]' > ~/.Xilinx/Vivado/Vivado_init.tcl
```

## Build project 01 from the command line

```bash
cd ~/microzed-linux/microzed/fpga/01-ps-only && vivado -mode batch -source build.tcl
```

Output: `~/vivado/mz01_ps_only/mz01_ps_only.bit` and `.xsa`. Open the project in the GUI with
`vivado ~/vivado/mz01_ps_only/mz01_ps_only.xpr`.

## Project 02: AXI register block

Register map (offsets from `0x43C00000`):

| Offset | Name | Access | Content |
|---|---|---|---|
| 0x00 | reg0 | R/W | scratch |
| 0x04 | reg1 | R/W | scratch |
| 0x08 | sum | R | reg0 + reg1, computed in the PL |
| 0x0C | count | R | +1 every FCLK_CLK0 clock (100 MHz) |
| 0x10 | id | R | `0x4D5A0201` |

Simulate first (optional, `sudo apt install iverilog`):

```bash
cd ~/microzed-linux/microzed/fpga/02-axiregs && iverilog -o /tmp/tb tb_axiregs.v axiregs.v && vvp /tmp/tb
```

Build:

```bash
cd ~/microzed-linux/microzed/fpga/02-axiregs && vivado -mode batch -source build.tcl
```

Output in `~/vivado/mz02_axiregs/`: `mz02_axiregs.bit`, `axiregs.bin` (for the board), `mz02_axiregs.xsa`.
`make overlay` / `make push` in `microzed/` copy `axiregs.bin` to `/lib/firmware/` on the board.

## Project 04: AXI DMA loopback

```
CPU --GP0--> axi_gp0_ic --> axiregs_0 (0x43C00000), axi_dma_0 registers (0x40400000)
DDR <--HP0-- axi_hp0_ic <-- axi_dma_0 MM2S (reads DDR) + S2MM (writes DDR)
axi_dma_0 MM2S stream --> axis_fifo (4 KB) --> S2MM stream
mm2s_introut -> IRQ 61, s2mm_introut -> IRQ 62
```

Simple mode (no scatter-gather), 32-bit data, buffer length register 23 bits (up to 8 MB per transfer).

Build:

```bash
cd ~/microzed-linux/microzed/fpga/04-dma-loopback && vivado -mode batch -source build.tcl
```

Output in `~/vivado/mz04_dma/`: `mz04_dma.bit`, `mz04_dma.bin`, `mz04_dma.xsa`, `utilization.txt`.
`make overlay` / `make push` in `microzed/` install `mz04_dma.bin` as `/lib/firmware/pl.bin`
(they fall back to project 02's `axiregs.bin` if project 04 is not built).
