# 01: Vivado setup

## Goal

Install AMD Vivado on the Mint laptop, create a MicroZed 7010 project, and build a first bitstream from a block design that contains only the Zynq PS.

## Why

Vivado is the tool for everything on the PL (FPGA) side. This project gets the tool working and checks the full build flow once, before any custom logic.

## Builds on

Nothing from the earlier work, except knowing the board: XC7Z010-1CLG400C (Vivado part `xc7z010clg400-1`), Avnet MicroZed, booting Buildroot from SD.

## Steps (outline)

1. Check the current Vivado release and its supported OS list (AMD UG973). Linux Mint 22 is based on Ubuntu 24.04, which recent releases list as supported. Download the Linux web installer from AMD (free AMD account needed).
2. Install prerequisites. Ubuntu 24.04 based systems need `libtinfo5` (Vivado uses `libtinfo.so.5`), which is no longer in the default repositories; follow a current guide for installing it, and do not remove `libncurses6`.
3. Run the installer: the free edition (Vivado ML Standard, formerly WebPACK) covers the Zynq-7010. Select only Zynq-7000 device support to save disk space. Vitis is not needed yet.
4. Install the Avnet board definition files (Avnet `bdf` repository on GitHub), so "MicroZed 7010" appears as a board and the PS gets the right DDR/MIO presets.
5. Create a project for the MicroZed 7010, add a block design with the Zynq7 Processing System, run block automation (apply board preset), create the HDL wrapper, generate the bitstream.
6. Note the output files: `.bit` (bitstream) and `.xsa` (hardware description).
7. Optional: load nothing yet. Just inspect the PS configuration (clocks: FCLK_CLK0; AXI GP0 master; interrupts) to prepare for project 02.

## Done when

- Vivado starts from the menu or with `vivado` in a terminal.
- A MicroZed 7010 project builds a bitstream with no errors.
- The project folder location and Vivado version are recorded in `buildroot-microzed-project.md`.

## Pitfalls

- The download and install are large (tens of GB). Check free disk space first (`df -h ~`).
- The installer can hang or fail on missing libraries; check the install log and the prerequisites.
- Keep Vivado projects out of the git repo (they are large and full of generated files). Only small sources (block design Tcl, HDL, constraints) belong in git, later.
- The board boots with Buildroot's own U-Boot SPL and PS setup (`ps7_init` from U-Boot), not with Vivado's. For now the plan is to load only the PL bitstream from Linux, so the Vivado PS settings must not conflict with what U-Boot set up (see project 02).
