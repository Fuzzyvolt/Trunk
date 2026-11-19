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
| `03-pltimer/pltimer.v` | Project 03: AXI4-Lite timer with a level interrupt output |
| `03-pltimer/tb_pltimer.v` | Testbench for it (passes in Icarus Verilog) |
| `03-pltimer/build.tcl` | Project 03: PS + axiregs (`0x43C00000`) + pltimer (`0x43C10000`), `irq` -> `IRQ_F2P[0]`; builds `mz03_pltimer.bin` (tested, works on the board) |
| `05-adc-stream/adcsim.v` | Project 05: simulated ADC: AXI4-Lite registers + AXI4-Stream output (sine or sawtooth, sample every DIV clocks, TLAST every PKT_LEN samples, drops counted) |
| `05-adc-stream/tb_adcsim.v` | Testbench for it (passes in Icarus Verilog) |
| `05-adc-stream/build.tcl` | Project 05: project 04 design with adcsim (`0x43C20000`) -> 8192-word FIFO -> DMA S2MM; axiregs ID `0x4D5A0501`; builds `mz05_adc.bin` (tested, works on the board) |
| `04-dma-loopback/build.tcl` | Project 04: the project 03 design (axiregs ID `0x4D5A0401`) + AXI DMA at `0x40400000` with an AXI4-Stream FIFO loop, HP0 to DDR, DMA interrupts on `IRQ_F2P[1..2]`; builds `mz04_dma.bin` (tested, works on the board) |

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

## Project 03: PL timer with interrupt

```
                                         +--> axiregs_0  0x43C00000 (project 02, kept)
CPU --M_AXI_GP0--> AXI Interconnect -----+
                                         +--> pltimer_0  0x43C10000
GIC <-- IRQ_F2P[0] = GIC ID 61 <-------------------- irq
```

Register map (offsets from `0x43C10000`):

| Offset | Name | Access | Content |
|---|---|---|---|
| 0x00 | CTRL | R/W | bit 0 EN (timer runs), bit 1 IE (interrupt output on) |
| 0x04 | PERIOD | R/W | clock cycles per tick (100000 = 1 ms); 0 = no ticks |
| 0x08 | STATUS | R/W1C | bit 0 PENDING; write 1 to clear (drops `irq`) |
| 0x0C | TICKS | R/W | ticks since reset; any write sets it to 0 |
| 0x10 | COUNT | R | position in the current period |
| 0x14 | ID | R | `0x4D5A0301` |

`irq` = PENDING and IE, from a flip-flop. It is a level interrupt: it stays high until the CPU clears PENDING.

Simulate:

```bash
cd ~/microzed-linux/microzed/fpga/03-pltimer && iverilog -o /tmp/tb tb_pltimer.v pltimer.v && vvp /tmp/tb
```

Build (about as long as project 02):

```bash
cd ~/microzed-linux/microzed/fpga/03-pltimer && vivado -mode batch -source build.tcl
```

Output in `~/vivado/mz03_pltimer/`: `mz03_pltimer.bit`, `mz03_pltimer.bin` (for the board), `mz03_pltimer.xsa`.
`make overlay` / `make push` in `microzed/` install it as `/lib/firmware/pl.bin` (unless project 04 is built); `/etc/init.d/S40pl` loads it at boot.

## Project 04: AXI DMA loopback

```
CPU --GP0--> axi_gp0_ic --> axiregs_0 (0x43C00000), pltimer_0 (0x43C10000), axi_dma_0 registers (0x40400000)
DDR <--HP0-- axi_hp0_ic <-- axi_dma_0 MM2S (reads DDR) + S2MM (writes DDR)
axi_dma_0 MM2S stream --> axis_fifo (4 KB) --> S2MM stream
irq_concat: In0 pltimer -> GIC 61, In1 mm2s_introut -> GIC 62, In2 s2mm_introut -> GIC 63
```

Simple mode (no scatter-gather), 32-bit data, buffer length register 23 bits (up to 8 MB per transfer).
Wired by hand (no connection automation), so every connection is one line in the script.

Build:

```bash
cd ~/microzed-linux/microzed/fpga/04-dma-loopback && vivado -mode batch -source build.tcl
```

Output in `~/vivado/mz04_dma/`: `mz04_dma.bit`, `mz04_dma.bin`, `mz04_dma.xsa`, `utilization.txt`.
`make overlay` / `make push` in `microzed/` install `mz04_dma.bin` as `/lib/firmware/pl.bin`
(they fall back to project 03's `mz03_pltimer.bin` if project 04 is not built).

## Project 05: DMA data stream (simulated ADC)

```
CPU --GP0--> axi_gp0_ic --> axiregs_0 (0x43C00000), pltimer_0 (0x43C10000),
                            axi_dma_0 registers (0x40400000), adcsim_0 (0x43C20000, new)
adcsim_0 m_axis --> axis_fifo (8192 x 32 bit) --> axi_dma_0 S2MM --> HP0 --> DDR
adcsim_0 fifo_aresetn --> axis_fifo reset (FLUSH empties the FIFO)
interrupts as in project 04 (S2MM done = GIC 63)
```

adcsim registers (offsets from `0x43C20000`):

| Offset | Name | Access | Content |
|---|---|---|---|
| 0x00 | CTRL | R/W | bit 0 EN, bit 1 SINE (0 = sawtooth), bit 2 FLUSH (FIFO reset, generator stopped) |
| 0x04 | STATUS | R | bit 0 RUNNING (stays 1 after EN=0 until the packet is complete) |
| 0x08 | DIV | R/W | clocks per sample (100 = 1 MSa/s) |
| 0x0C | PKT_LEN | R/W | samples per packet (TLAST) = one DMA buffer |
| 0x10 | PHASE_INC | R/W | frequency = rate x PHASE_INC / 2^32 |
| 0x14 | SAMPLES | R | samples made since start |
| 0x18 | DROPPED | R | samples dropped (stream not ready when one was due) |
| 0x1C | PACKETS | R | packets sent |
| 0x20 | ID | R | `0x4D5A0501` |

Sample word: bits 31..16 sequence number (+1 per sample, dropped ones included), bits 15..0 signed value.

Simulate:

```bash
cd ~/microzed-linux/microzed/fpga/05-adc-stream && iverilog -o /tmp/tb tb_adcsim.v adcsim.v && vvp /tmp/tb
```

Build:

```bash
cd ~/microzed-linux/microzed/fpga/05-adc-stream && vivado -mode batch -source build.tcl
```

Output in `~/vivado/mz05_adc/`: `mz05_adc.bit`, `mz05_adc.bin`, `mz05_adc.xsa`, `utilization.txt`.
`make overlay` / `make push` install `mz05_adc.bin` as `/lib/firmware/pl.bin` (newest built design wins: 05, then 04, then 03).
