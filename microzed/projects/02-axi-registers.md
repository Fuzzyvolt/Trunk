# 02: AXI register block

## Goal

Put a small custom AXI4-Lite peripheral with a few registers in the PL, load its bitstream from Linux, read and write the registers from the shell, then write a platform driver for it.

## Why

This is the "hello world" of Zynq: it covers the whole flow (Vivado design -> bitstream -> load from Linux -> device tree node -> driver) in its simplest form. Every later PL project uses the same steps.

## Builds on

- 01 (Vivado works).
- `myled-irq` driver: platform driver, `compatible`, `probe()`, `devm_*` (see `microzed/modules/`).
- `post-image-dt.sh`: how we add nodes to the `.dtb`.

## Steps (outline)

1. In Vivado, create a custom AXI4-Lite IP (Tools -> Create and Package New IP) with 4 registers. Make one register read-only and computed in logic (for example a free-running counter, or reg0 + reg1), so a read proves the logic runs.
2. Add it to the block design on M_AXI_GP0. Note its address (typically `0x43C00000`). Generate the bitstream.
3. Find out how this kernel loads bitstreams: `ls /sys/class/fpga_manager/` and whether `fpga0/firmware` exists (Xilinx kernels have it; mainline uses device tree overlays). Alternative: load from U-Boot (`fpga load`). Pick one and document it.
4. Convert the bitstream if needed (the FPGA manager usually wants a `.bin` made with `bootgen ... -process_bitstream bin`), copy it to `/lib/firmware/` on the board, load it, check the blue DONE LED.
5. Test with `devmem` (BusyBox): write reg0, read it back, read the computed register.
6. Add a device tree node (`compatible = "microzed,axiregs"`, `reg = <0x43c00000 0x1000>`, PL clock) with `post-image-dt.sh`.
7. Write `microzed/modules/axiregs/`: platform driver with `devm_platform_ioremap_resource()`, `readl()`/`writel()`, and sysfs attributes (for example `/sys/.../reg0`) to read and write registers.
8. Show a register on the web page.

## Done when

- `devmem 0x43c00000` reads back what was written, and the computed register changes as expected.
- The driver binds to the node and its sysfs files read and write the registers.
- Bitstream loading is scripted (or in an init script) and documented in the md.

## Pitfalls

- **Touching PL addresses before the bitstream is loaded (or with its clock off) hangs the board.** Load first, then access.
- The PL needs a clock (FCLK_CLK0). If U-Boot's `ps7_init` does not enable it, enable it from Linux: give the node `clocks = <&clkc 15>` (fclk0) and call `clk_prepare_enable()` in the driver, or check `/sys/kernel/debug/clk/`.
- The PS-PL level shifters must be enabled; the Linux FPGA manager normally does this after loading.
- Keep generated Vivado files out of git; commit the HDL, a block design Tcl export and the `.bin`/`.bit` only if small.
