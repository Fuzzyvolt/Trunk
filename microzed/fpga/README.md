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
`make overlay` / `make push` in `microzed/` copy `mz03_pltimer.bin` to `/lib/firmware/`; `/etc/init.d/S40pl` loads it at boot.
