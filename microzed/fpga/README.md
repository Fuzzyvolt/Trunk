# FPGA (PL) sources

Only small sources live here (Tcl, HDL, constraints). The Vivado projects themselves are generated
outside the repo, in `~/vivado/<name>`, because they are large and full of generated files.

| Folder | What it is |
|---|---|
| `01-ps-only/build.tcl` | Project 01: block design with only the Zynq PS, builds `.bit` and `.xsa` |

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
