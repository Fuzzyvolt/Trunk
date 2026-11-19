# Buildroot + QEMU + MicroZed: project summary

Last updated: 2026-11-19 (project 06 files written, not yet tested on the laptop/board)

Load this file at the start of a new session to continue the project.

Workflow: Claude runs on a Windows PC. The user copies commands from the chat to the Mint PC with KDE Connect (shared clipboard), so give commands in chat, one per fenced code block. No need to write them to a file. `buildroot-qemu-commands.txt` in the same folder is an older reference; the block names below (N3, N7...) refer to it.

## Goal

Learn embedded Linux hands-on: build a Linux system with Buildroot, run it in QEMU, then bring it up on real hardware (Avnet MicroZed). Later goals are kernel drivers, FPGA (PL) designs, AXI DMA and FreeRTOS. These cover gaps in real DMA, RTOS and driver experience.

The learner has little hands-on embedded Linux experience. Explain from the basics, with diagrams first.

## Hardware and host

| Item | Details |
|---|---|
| Build PC | Linux Mint laptop (ASUS UX430UAR), user `fabio`, x86_64 |
| Target board | Avnet MicroZed, Zynq-7010 (XC7Z010), 2x Cortex-A9 (32-bit ARMv7), 1 GiB DDR3, 32 MB QSPI (S25FL256S), microSD, Gigabit Ethernet (Marvell 88E1512) |
| Serial console | Micro-USB J2 (power + CP210x USB-UART) appears as `/dev/ttyUSB0` on the PC, 115200 8N1, using `picocom` |
| SD card | 14.4 GB microSD, shows as `/dev/sdb` on the PC (always check with `lsblk`; `/dev/sda` is the PC's own drive) |
| Reference | `Avnet-MicroZed-7Z010-7Z020-EN-User-Guide.pdf` (in the Windows Downloads folder) |

### MicroZed facts confirmed from the Avnet user guide

| Item | Pin / setting |
|---|---|
| User LED D3 | MIO47 (1.8 V bank), active high |
| User button PB1 | MIO51 (1.8 V bank), pull-down: released 0, pressed 1 |
| Pmod header J5 | MIO 0, 9-15 (3.3 V), connected to the PS side |
| Boot: QSPI (factory) | JP3 2-3, JP2 1-2, JP1 1-2 |
| Boot: SD card (current) | JP3 2-3, **JP2 2-3**, JP1 1-2 |
| Reset button SW2 | Does **not** re-read the boot jumpers; power-cycle after changing them |
| Ethernet PHY | Marvell 88E1512, RGMII on MIO 16-27, MDC = MIO52, MDIO = MIO53 (1.8 V bank 501), own 25 MHz oscillator |
| PHY reset (ETH_RST_N, PHY pin 16) | **No MIO pin.** Driven by PG_MODULE (3.3 V power-good = board power-on reset) through level translator U8. Software cannot reset the PHY |
| SW2 (PS_SRST#) | Resets the processor and PL only, **not** the PHY |
| USB PHY reset | MIO7 (AND-ed with PG_MODULE) |

The factory QSPI image (U-Boot 2013.01 and an old Linux) is still in flash, untouched.

## Software on the PC

- Host packages installed: `build-essential git wget cpio unzip rsync bc file libncurses-dev qemu-system-arm picocom`
- User `fabio` added to the `dialout` group (needed to open `/dev/ttyUSB0`)
- Buildroot source: `~/buildroot`, cloned from `https://gitlab.com/buildroot.org/buildroot.git`, checked out on the LTS branch `2026.02.x` (branch not re-verified on the PC)

## The two builds

**QEMU is dropped as of 2026-11-14.** The user works on the MicroZed only. `~/buildroot` stays, because it is the Buildroot source tree (and download cache `dl/`) that the MicroZed build uses. Only its `output/` folder, the QEMU build, is unused. QEMU could still come back later for kernel debugging with gdb (QEMU can emulate a Zynq with `-M xilinx-zynq-a9`), but it isn't needed now.

Each build has its own `.config`. Running `make menuconfig` in the wrong folder edits the wrong build (this has already happened once).

| | QEMU build | MicroZed build |
|---|---|---|
| Folder | `~/buildroot` (in-tree, output in `~/buildroot/output`) | `~/br-microzed` (out-of-tree, made with `make O=$HOME/br-microzed ...`) |
| Defconfig | `qemu_aarch64_virt_defconfig` | `zynq_microzed_defconfig` |
| Arch | AArch64, Cortex-A53 | ARM 32-bit, Cortex-A9 |
| Images | `~/buildroot/output/images/` (`Image`, `rootfs.ext4`, `start-qemu.sh`) | `~/br-microzed/images/` (`boot.bin`, `u-boot.img`, kernel, `.dtb`, `sdcard.img`) |
| Status | Built. Booting in QEMU not confirmed in session. Its `.config` now also has dropbear, libgpiod2, root password and DHCP (added by mistake); it has not been rebuilt since. | Boots from SD to a login prompt. See "Current state" below. |

Downloads in `~/buildroot/dl/` are shared by both builds.

### MicroZed boot chain (working)

```
BootROM (jumpers = SD)
  -> boot.bin   U-Boot SPL 2025.01  (clocks, DDR init)
  -> u-boot.img U-Boot 2025.01      (Model: Avnet MicroZed board)
  -> /extlinux/extlinux.conf on SD partition 1, entry "linux"
  -> kernel + device tree
  -> rootfs on SD partition 2 -> BusyBox init -> "buildroot login:"
```

SD card layout from `sdcard.img`: `sdb1` 32 MB FAT (boot files), `sdb2` 60 MB ext4 (rootfs). The rest of the card is unused.

Harmless boot messages:
- `spl_load_image_fat: error reading image uImage, err -22`: SPL tries Falcon mode first, then falls back to U-Boot.
- `bad env area, using default environment`: no saved U-Boot environment yet.
- `using random MAC address`: no stored MAC. A fixed one can be set in U-Boot with `setenv ethaddr 02:0a:35:00:00:01; saveenv` (erased on every re-flash).

## Current state (where we stopped)

As of 2026-11-18: **GPIO, SSH (key login), cross-compiled programs, rootfs overlay, a status web page with LED/power-off buttons, and our own kernel driver (`myled_irq`, loaded at boot) all work. Project 02 works too: our own AXI register block in the PL, programmed from Linux at boot, with a driver and sysfs files (see "AXI register block" below). Project 03 works: a timer in the PL interrupts Linux every N µs, `/dev/pltimer` blocks until the next tick (see "PL timer with interrupt" below). Project 04 works: an AXI DMA loops buffers DDR -> PL FIFO -> DDR through the kernel's DMA engine framework, up to 393 MB/s (see "AXI DMA loopback" below). Project 05 works: a simulated ADC in the PL streams samples through the DMA into a ring of DDR buffers, read from `/dev/adcstream`, with a live web chart; zero loss at 1 MSa/s for 5 minutes; with every sample checked, no loss up to 25 MSa/s; zero-copy (mmap) reaches the DMA's 98 MSa/s (see "DMA data stream" below).** Work has moved to a **second MicroZed**, because the first board's Ethernet PHY is unreliable (see below). Kernel modules `hello`, `myled` (character driver), `myled-dt` (device tree platform driver) and `myled-irq` (button interrupt, blocking `/dev/mybutton`) also work.

1. `~/br-microzed/.config` has (to add in project 06: `BR2_EXTERNAL` = `/home/fabio/microzed-linux/microzed/br2-external`, `BR2_PACKAGE_MICROZED_APPS=y`, `BR2_PACKAGE_MICROZED_MODULES=y`): kernel config fragment `/home/fabio/microzed-linux/microzed/configs/linux.fragment` (`CONFIG_XILINX_DMA=m`), `dropbear`, **libgpiod v1** (`BR2_PACKAGE_LIBGPIOD=y`, `BR2_PACKAGE_LIBGPIOD_TOOLS=y`), **ethtool**, a root password, `BR2_SYSTEM_DHCP="eth0"`, `BR2_ROOTFS_OVERLAY="/home/fabio/mz-overlay"`, and the BusyBox fragment `/home/fabio/mz-busybox.fragment`.
2. GPIO: `gpiodetect` shows `gpiochip0 [zynq_gpio] (118 lines)` (lines 0-53 = MIO, 54-117 = EMIO). LED D3 and button PB1 are owned by the `myled_irq` kernel driver (`/dev/myled`, `/dev/mybutton`), loaded at boot by `/etc/init.d/S30myled`.
3. Network: fixed MAC `02:0a:35:00:00:01`, so the board gets **10.0.0.134**. `ssh root@10.0.0.134` logs in with a key (no password).
4. Web page: `http://10.0.0.134` (redirects to `/cgi-bin/status`): die temperature, all XADC voltage rails, uptime, load, memory, IP, kernel, LED state, and buttons (LED on/off/toggle, power off). Refreshes every 5 s.
5. Config backup: `~/mz-overlay-microzed.config` (copy of `.config`).

### Vivado (project 01, done 2026-11-15)

- **Vivado 2026.1**, edition **Vivado Basic** (free), installed in `/tools/Xilinx/2026.1/Vivado` (folder made with `sudo mkdir -p /tools/Xilinx && sudo chown fabio: /tools/Xilinx`, installer run as the normal user). Devices: Zynq-7000 only. 21 GB download, about 80 GB on disk. Vitis, Model Composer and cable drivers not installed (the MicroZed has no on-board USB-JTAG).
- Load it in a terminal with `vivado-env` (alias in `~/.bashrc` for `source /tools/Xilinx/2026.1/Vivado/settings64.sh`); the menu entry works without it. Not sourced in every shell on purpose, so AMD's tools stay out of the Buildroot builds.
- **License:** from 2026.1, Vivado will not start without a license file, even the free tier. Free **Vivado Basic Tier, node-locked**, generated on the AMD licensing site (VLM -> Obtain License -> "Get My Full or Purchased Certificate-Based License" -> Connect Now), host ID = the laptop's Wi-Fi MAC (added as a new host; the "Farbauti" hosts already on the AMD account are another PC). Loaded with VLM -> Load License -> Copy License. **Valid 12 months, renew for free.** Basic does not cover patch releases (2026.1.x).
- Extra libraries for Mint 22 (from Ubuntu 22.04, `security.ubuntu.com/ubuntu/pool/universe/n/ncurses/`): `libtinfo5_6.3-2ubuntu0.3_amd64.deb` and `libncurses5_6.3-2ubuntu0.3_amd64.deb`, installed with `sudo apt install ./<file>.deb`. Also ran AMD's `Vivado/scripts/installLibs.sh` (it detects `linuxmint-22`).
- **Board files:** Avnet `bdf` repo cloned to `~/vivado-boards/avnet-bdf`; `~/.Xilinx/Vivado/Vivado_init.tcl` contains `set_param board.repoPaths [list /home/fabio/vivado-boards/avnet-bdf]`. Board part `avnet-tria:microzed_7010:part0:1.4` (DDR3 MT41K256M16 RE-125). A `CRITICAL WARNING [Board 49-56]` about `zub1cg/1.2/board.xml` (another Avnet board) is harmless.
- **Projects live outside git**, in `~/vivado/`:
  - `~/vivado/mz01_gui`: made by hand in the GUI. Block design `system` with only the ZYNQ7 PS, block automation with the board preset, HDL wrapper, bitstream. Outputs `mz01_gui.runs/impl_1/system_wrapper.bit` and `system_wrapper.xsa`.
  - `~/vivado/mz01_ps_only`: the same design built by `microzed/fpga/01-ps-only/build.tcl` (`vivado -mode batch -source build.tcl`), works. Writes `mz01_ps_only.bit` and `.xsa` in the project folder.
- PS settings from the board preset (needed for project 02): FCLK_CLK0 = 100 MHz, M_AXI_GP0 off, fabric interrupts off.
- Flow: block design -> HDL wrapper -> Generate Bitstream (runs synthesis and implementation itself if needed) -> File -> Export -> Export Hardware, "Include bitstream" (`.xsa`). Export fails with `[Common 17-69] Unable to get BIT file` if the bitstream run has not finished yet or is out of date.
- Project 02 (below) loads its bitstream from Linux; nothing is in QSPI or `boot.bin`.

### AXI register block (project 02, done 2026-11-17)

```
CPU (PS) --M_AXI_GP0 (AXI3)--> AXI Interconnect --(AXI4-Lite)--> axiregs (PL)
                               (protocol converter)              0x43C00000, 4 KB
FCLK_CLK0 (100 MHz) clocks the PL side; proc_sys_reset makes the reset.
```

| Offset | Name | Access | Content |
|---|---|---|---|
| 0x00 | reg0 | R/W | scratch |
| 0x04 | reg1 | R/W | scratch |
| 0x08 | sum | R | reg0 + reg1, computed in the PL |
| 0x0C | count | R | +1 per 100 MHz clock (wraps every ~43 s) |
| 0x10 | id | R | `0x4D5A0201` |

Files (all in `microzed/`):
- `fpga/02-axiregs/axiregs.v`: hand-written AXI4-Lite slave. Added to the block design as a **module reference** (`create_bd_cell -type module -reference axiregs`), not with "Create and Package New IP": one readable file in git, no generated boilerplate. The `s_axi_*` port names make Vivado see them as one AXI interface; `X_INTERFACE_*` attributes tie the clock and reset to it.
- `fpga/02-axiregs/tb_axiregs.v`: testbench (the AXI master role). `iverilog -o /tmp/tb tb_axiregs.v axiregs.v && vvp /tmp/tb` prints PASS lines.
- `fpga/02-axiregs/build.tcl`: PS with board preset + `PCW_USE_M_AXI_GP0 1`, axiregs, connection automation (AXI Interconnect + Processor System Reset), address `0x43C00000`/4K, bitstream, `axiregs.bin`, `.xsa`. Works with Vivado 2026.1: `cd ~/microzed-linux/microzed/fpga/02-axiregs && vivado -mode batch -source build.tcl`. Output in `~/vivado/mz02_axiregs/`.
- `fpga/02-axiregs/bit2bin.tcl`: `.bit` -> `.bin` for the FPGA manager. A `.bit` is a text header (design, part, date) + config data; the PCAP wants only the data with each 32-bit word byte-swapped, so the sync word `AA 99 55 66` reads `66 55 99 AA`. Replaces `bootgen -process_bitstream bin`, which needs Vitis (not installed).
- `modules/axiregs/axiregs.c`: platform driver, `compatible = "microzed,axiregs"`.
- `board/post-image-dt.sh`: now also adds `axiregs@43c00000 { compatible; reg = <0x43c00000 0x1000>; clocks = <&clkc 15>; fpga-mgr = <&devcfg>; }`. The script has a `phandle()` helper (clkc and devcfg already have phandles, the GPIO controller gets one).
- `overlay/etc/init.d/S40axiregs`: programs the PL, then `insmod`s the driver. `make overlay`/`make push` install `/lib/firmware/axiregs.bin` (from `~/vivado/mz02_axiregs/`, skipped with a warning if missing) and `/lib/modules/axiregs/axiregs.ko`.
- Web page: "PL registers" table (id, reg0, reg1, sum, count).

Loading the bitstream (Xilinx kernel FPGA manager, sysfs):

```bash
echo 0 > /sys/class/fpga_manager/fpga0/flags             # 0 = full bitstream
echo axiregs.bin > /sys/class/fpga_manager/fpga0/firmware  # read from /lib/firmware/
cat /sys/class/fpga_manager/fpga0/state                  # "operating"; blue DONE LED on
```

`fpgautil -b /lib/firmware/axiregs.bin` (in the Buildroot defconfig) does the same. The kernel driver (`zynq-fpga`) also handles the PS-PL level shifters and the PL resets during the load.

Test with BusyBox `devmem` (only once `state` is `operating`):

```bash
devmem 0x43C00010            # 0x4D5A0201
devmem 0x43C00000 32 5       # reg0 = 5
devmem 0x43C00004 32 7       # reg1 = 7
devmem 0x43C00008            # 0x0000000C
```

Driver (after `make flash`):

```bash
dmesg | grep axiregs         # "axiregs at ..., ID 0x4d5a0201, PL clock 100000000 Hz"
cd /sys/bus/platform/devices/43c00000.axiregs
echo 5 > reg0; echo 0x10 > reg1; cat sum   # 21
cat count; sleep 1; cat count              # +~100 million
```

How the driver works:
- `probe()` first checks the PL is programmed: `of_parse_phandle(np, "fpga-mgr")` -> `of_fpga_mgr_get()` -> `mgr->state == FPGA_MGR_STATE_OPERATING`, else it fails with "load the bitstream first" instead of hanging the board. Then it reads `id` and refuses a wrong bitstream.
- `devm_clk_get_enabled()` claims FCLK_CLK0 (unclaimed clocks can be turned off by the kernel); `devm_platform_get_and_ioremap_resource()` maps the `reg` range; `readl()`/`writel()` access it.
- sysfs files come from `DEVICE_ATTR_RW/RO` + `ATTRIBUTE_GROUPS` in `.dev_groups`; the driver core creates them after `probe()` succeeds. `kstrtou32(buf, 0, ...)` accepts decimal or `0x` hex. Values print in decimal, `id` in hex.

Facts found on the way:
- The kernel is **linux-xlnx** (`xlnx_rebase_v6.12_LTS`, `xilinx_zynq_defconfig`), U-Boot is **u-boot-xlnx** (Buildroot 2026.02.x `zynq_microzed_defconfig`). The Xilinx kernel has the FPGA manager `firmware` sysfs file; mainline Linux would need device tree overlays instead.
- U-Boot SPL uses its own `ps7_init` (board/xilinx/zynq/zynq-microzed), not Vivado's: `FPGA0_CLK_CTRL = 0x00100A00` = IO PLL (1000 MHz) / 10 = **100 MHz FCLK0**. PS settings changed in Vivado (clock rates, MIO) do not reach the board unless `boot.bin` is rebuilt from the `.xsa`; only the PL bitstream is used.
- The Xilinx `zynq-7000.dtsi` has `fclk-enable = <0xf>`, so all four FCLKs are on from boot. That is why `devmem` works before the driver loads.

### PL timer with interrupt (project 03, done 2026-11-17)

```
                                         +--> axiregs_0  0x43C00000 (project 02, kept)
CPU --M_AXI_GP0--> AXI Interconnect -----+
                                         +--> pltimer_0  0x43C10000
GIC <-- IRQ_F2P[0] = GIC ID 61 <-------------------- irq (level high)
```

| Offset | Name | Access | Content |
|---|---|---|---|
| 0x00 | CTRL | R/W | bit 0 EN (timer runs), bit 1 IE (interrupt output on) |
| 0x04 | PERIOD | R/W | clock cycles per tick (100000 = 1 ms); 0 = no ticks |
| 0x08 | STATUS | R/W1C | bit 0 PENDING; write 1 to clear (drops `irq`) |
| 0x0C | TICKS | R/W | ticks since reset; any write sets it to 0 |
| 0x10 | COUNT | R | position in the current period |
| 0x14 | ID | R | `0x4D5A0301` |

Files (all in `microzed/`):
- `fpga/03-pltimer/pltimer.v`: same AXI4-Lite skeleton as `axiregs.v`, plus the timer. `irq` = PENDING and IE, registered (from a flip-flop, so it cannot glitch). `X_INTERFACE_INFO ... interrupt ... SENSITIVITY LEVEL_HIGH` makes Vivado see it as an interrupt pin.
- `fpga/03-pltimer/tb_pltimer.v`: testbench (passes in Icarus): level stays high until W1C, IE gating, spacing = PERIOD clocks, PERIOD shrink, PERIOD 0.
- `fpga/03-pltimer/build.tcl`: project 02's script + `PCW_USE_FABRIC_INTERRUPT 1`, `PCW_IRQ_F2P_INTR 1`, both blocks on one interconnect (second automation call reuses `/ps7_axi_periph`), `connect_bd_net pltimer_0/irq ps7/IRQ_F2P` (one source needs no Concat block). Output `~/vivado/mz03_pltimer/mz03_pltimer.bin`. Worked first time with Vivado 2026.1.
- `modules/pltimer/pltimer.c`: platform driver, `compatible = "microzed,pltimer"`. `/dev/pltimer` is a **misc device** (`misc_register`: one call gives a `/dev` node, no cdev/class). Everything else is `devm_*` or `devm_add_action_or_reset`, so there is no `remove()`.
- `board/post-image-dt.sh`: adds `pltimer@43c10000 { compatible; reg = <0x43c10000 0x1000>; interrupt-parent = <&intc>; interrupts = <0 29 4>; clocks = <&clkc 15>; fpga-mgr = <&devcfg>; }` (intc = `/axi/interrupt-controller@f8f01000`).
- `overlay/etc/init.d/S40pl` **replaces `S40axiregs`**: loads `mz03_pltimer.bin`, then `insmod` axiregs and pltimer. `make overlay`/`make push` remove the old script from the overlay, `target/` and the board. Only one bitstream can be loaded at a time, so axiregs is kept in the project 03 design and the project 02 bitstream is no longer installed.
- Web page: "PL timer" table (running, period, interrupts, PL ticks, missed, interval stats, `/proc/interrupts` count).

On the board:

```bash
cat /dev/pltimer                      # "tick 42 pl 42 t 1234.567890" per tick, Ctrl-C to stop
cd /sys/bus/platform/devices/43c10000.pltimer
echo 100000 > period_us               # 100 ms (range 20 us .. ~42.9 s); default 1 s (module param period_us)
echo 0 > enable; echo 1 > enable      # stop / start
cat ticks pl_ticks missed stats       # write anything to stats to reset it
grep pltimer /proc/interrupts
```

Result at 100 ms: `ticks` 331 = `pl_ticks` 331, `missed` 0; `stats`: 170 intervals, min 99.996961 ms, avg 99.999979 ms, max 100.003369 ms, so **about ±3.5 µs jitter** at the handler. `/proc/interrupts`: ` 47: 331 0 GIC-0 61 Level pltimer`, so Linux IRQ 47, GIC ID 61, level, all on CPU0 (default affinity).

**Interrupt numbering:**
- GIC IDs: 0-15 SGI (CPU to CPU), 16-31 PPI (per-CPU, like the private timers), 32+ SPI (shared, peripherals).
- Device tree `interrupts = <type number flags>`: type 0 = SPI, number counted from 32 (so 29 = ID 61), flags 1 = rising edge, 4 = level high.
- Zynq: IRQ_F2P[0..7] = IDs 61-68, IRQ_F2P[8..15] = IDs 84-91.
- Linux uses its own IRQ numbers (47 here), which `platform_get_irq()` returns. `/proc/interrupts` shows both: Linux number first, then `GIC-0 61`.

**Top half vs threaded handler:**
- myled-irq used a threaded handler because it had to sleep (`msleep` for debounce, mutex, GPIO calls that may sleep).
- pltimer only does `readl`/`writel`, a few additions and `wake_up_interruptible()`, all allowed in interrupt context, so a plain top half (`devm_request_irq`) is enough and has the least latency. Shared state (counters, timestamps, stats) uses a **spinlock**: `spin_lock` in the handler, `spin_lock_irq` in `read()`/sysfs (interrupts off while holding it, or the handler could interrupt the holder on the same CPU and deadlock).
- The handler **must clear PENDING**: it is a level interrupt, so otherwise the GIC calls it again as soon as it returns and the board locks up. After the clear (`writel`) it reads a register back: writes to a device can be "posted" (the CPU moves on before they arrive), and the read makes sure the line is low before the handler returns. If STATUS is 0 the handler returns `IRQ_NONE` (not ours).
- Missed ticks: the PL counts every tick in TICKS; if two arrive between two interrupts they merge into one PENDING, and the handler adds `delta - 1` to `missed`. A slow *reader* is different: it sees the tick number jump.
- 64-bit division on the 32-bit ARM must use `div_u64()` / `div_u64_rem()` (`/` on a `u64` fails to link: `__aeabi_uldivmod`).

### AXI DMA loopback (project 04, done 2026-11-18)

```
                 M_AXI_GP0
CPU (PS) -----------------> axi_gp0_ic --+--> axiregs_0       0x43C00000 (ID now 0x4D5A0401)
                                         +--> pltimer_0       0x43C10000 (project 03, unchanged)
                                         +--> axi_dma_0 regs  0x40400000, 64 KB

DDR <-- S_AXI_HP0 <-- axi_hp0_ic <--+-- axi_dma_0 M_AXI_MM2S (reads tx_buf)
                                    +-- axi_dma_0 M_AXI_S2MM (writes rx_buf)

axi_dma_0 M_AXIS_MM2S --> axis_fifo (1024 x 32 bit) --> S_AXIS_S2MM

irq_concat: In0 pltimer -> IRQ_F2P[0] = GIC 61 (DT <0 29 4>)
            In1 mm2s_introut -> IRQ_F2P[1] = GIC 62 (<0 30 4>)
            In2 s2mm_introut -> IRQ_F2P[2] = GIC 63 (<0 31 4>)
```

AXI DMA settings: simple mode (no scatter-gather), 32-bit memory and stream data, 16-beat bursts, buffer length register **23 bits** (Vivado default is 14 = 16 KB max per transfer; the DT property `xlnx,sg-length-width = <23>` must match). HP0 left at 64 bits (its reset value, so U-Boot's `ps7_init` needs nothing); the interconnect converts 32 -> 64. Both DMA address windows cover all 1 GB of DDR (a smaller window makes the interconnect reject buffers above it).

Files (all in `microzed/`):
- `fpga/04-dma-loopback/build.tcl`: the project 03 design + DMA, **wired by hand** (no connection automation: one line per wire, fixed cell names). Uses `axiregs.v` and `pltimer.v` from their folders; sets the axiregs `ID` parameter to `0x4D5A0401`; checks after validation that the FIFO passes TLAST. Output `~/vivado/mz04_dma/mz04_dma.bin`. Worked first time with Vivado 2026.1.
- `configs/linux.fragment`: `CONFIG_XILINX_DMA=m`. Set in Buildroot menuconfig -> Kernel -> "Additional configuration fragment files", applied with `make linux-reconfigure && make`.
- `board/post-image-dt.sh`: adds `dma@40400000` (two channel subnodes, `xlnx,axi-dma-mm2s-channel` / `-s2mm-channel`, `xlnx,datawidth = <32>`, clocks named `s_axi_lite_aclk`, `m_axi_mm2s_aclk`, `m_axi_s2mm_aclk`) and `dmaloop { compatible = "microzed,dmaloop"; dmas = <&axi_dma 0>, <&axi_dma 1>; dma-names = "tx", "rx"; }`. Writes both with `status = "disabled"` and a warning unless the kernel `.config` has `CONFIG_XILINX_DMA=m`.
- `modules/dmaloop/dmaloop.c`: DMA engine client (see below). sysfs in `/sys/bus/platform/devices/dmaloop/`: `size` (4 B .. 4 MiB, multiple of 4, default 64 KiB), `run` (write N: N loopbacks, each checked; blocks until done), `result` (`ok size= count= avg_us= min_us= max_us= MBps=` or `FAIL ...`).
- `overlay/usr/bin/dmaloop-bench`: table for 1 KB .. 4 MB.
- `overlay/etc/init.d/S40pl`: loads `/lib/firmware/pl.bin`, insmod axiregs + pltimer, then **only if axiregs `id` is `0x4d5a04xx`**: `modprobe xilinx_dma`, insmod dmaloop. Stop unloads in reverse order.
- `Makefile`: `FPGA_BIN` = `mz04_dma.bin` if built, else `mz03_pltimer.bin`, installed as **`/lib/firmware/pl.bin`** (old `axiregs.bin`/`mz03_pltimer.bin` removed from overlay, `target/` and board). `make` also builds `dmaloop.ko`.
- `modules/axiregs/axiregs.c`: accepts any ID `0x4d5aXXXX` and prints the project number.
- Web page: "DMA loopback, last run" line.

**Why `CONFIG_XILINX_DMA=m`:** Xilinx's `xilinx_zynq_defconfig` has it `=y`. Built in, the driver binds at boot as soon as the DT node exists, enables clocks and reads the DMA status register (`DMASR`, to see whether SG is included) and resets the channels, all before the PL is programmed, so the CPU hangs on the AXI read. As a module, `S40pl` loads it after the bitstream. Buildroot runs `depmod` for kernel modules, so `modprobe xilinx_dma` works (unlike our own modules, which need `insmod` with a path).

How the client driver works (DMA engine framework):
- `dma_request_chan(dev, "tx")` / `"rx"`: looks the name up in `dma-names`, follows the matching `dmas` entry to the DMA node, asks its driver (`xilinx_dma`, `of_dma_xilinx_xlate`) for channel N. In xilinx_dma the MM2S channel is id 0 and S2MM id 1, whatever the subnode order. If xilinx_dma is not loaded: `-EPROBE_DEFER`. Holding the channels pins `xilinx_dma` (`rmmod xilinx_dma` fails while dmaloop is loaded).
- Buffers: `dma_alloc_coherent()` on **the DMA controller's device** (`chan->device->dev`, the one that does the memory access), 2 x 4 MiB from CMA (default 16 MiB). The HP port bypasses the CPU caches; coherent memory is uncached for the CPU, so both sides see the same data. The alternative is `kmalloc` + `dma_map_single()` + sync calls.
- Per transfer: `dmaengine_prep_slave_single()` (descriptor: buffer, length, direction `DMA_DEV_TO_MEM` / `DMA_MEM_TO_DEV`) -> set `callback_result` -> `dmaengine_submit()` (queues only) -> `dma_async_issue_pending()` (starts). **RX (S2MM) first**, then TX. The callback runs in xilinx_dma's tasklet after the DMA interrupt and calls `complete()`; the client sleeps in `wait_for_completion_timeout()` (1 s, then `dmaengine_terminate_sync()`). `result->result` reports DMA errors.
- Check: a pattern that changes per word and per transfer (`seed ^ (w * 0x9e3779b1)`), rx cleared before each transfer, `memcmp` after. Only the DMA part is timed (`ktime_get()`).
- In simple mode xilinx_dma programs one buffer per descriptor (address + length register `BTT`, writing BTT starts it), so the maximum per transfer is the length register width.

Results (board 2, 2026-11-18), boot log:

```
axiregs 43c00000.axiregs: ... ID 0x4d5a0401 (project 04 design), PL clock 99999999 Hz
xilinx-vdma 40400000.dma: Xilinx AXI DMA Engine Driver Probed!!
dmaloop dmaloop: tx dma1chan1, rx dma1chan0, buffers tx 0x3f100000 rx 0x3f500000 (4194304 bytes each)
```

(`dma0` is the PS's own PL330 DMA. The buffers at ~0x3F000000 are near the top of the 1 GB DDR, so the 1 GB address window matters.)

`dmaloop-bench` (100 transfers per size, all data correct):

| Bytes | avg µs | MB/s |
|---|---|---|
| 1 KiB | 39 | 25 |
| 4 KiB | 46 | 87 |
| 16 KiB | 77 | 211 |
| 64 KiB | 202 | 323 |
| 256 KiB | 695 | 376 |
| 1 MiB | 2663 | 393 |

- Ceiling: 32 bits x 100 MHz = 400 MB/s; 1 MiB reaches 98% of it.
- Model: time ≈ **36 µs fixed** + bytes / 400 MB/s (64 KiB: 36 + 164 = 200 µs, measured 202). The fixed part is descriptor setup, two interrupts, the tasklet and waking the sleeping writer. Small transfers are dominated by it; real streams use large buffers or scatter-gather (many buffers queued, no CPU in between).
- The 4 MiB row did not show in the pasted output (100 x 4 MiB takes about a minute because the CPU fill/clear/compare runs on uncached memory; not timed). Not yet re-checked.

### DMA data stream, simulated ADC (project 05, done 2026-11-19)

```
 PL                                                          PS
 adcsim_0 (0x43C20000) --stream--> axis_fifo --> axi_dma_0 S2MM --HP0--> ring of 8 DDR buffers
  1 sample every DIV clocks         8192 x 32    (xilinx_dma)            |
  TLAST every PKT_LEN samples                                            +--> /dev/adcstream --> adcread
  fifo_aresetn --> FIFO reset (FLUSH)                                    +--> sysfs snapshot --> /scope.html
```

The project 04 design with the FIFO's input moved: `adcsim` feeds S2MM; the DMA's MM2S side is kept (unused) so the DMA still matches its device tree node. axiregs ID `0x4D5A0501`. Built first time with Vivado 2026.1.

adcsim registers (`fpga/05-adc-stream/adcsim.v`, offsets from `0x43C20000`): CTRL (bit 0 EN, bit 1 SINE, bit 2 FLUSH), STATUS (bit 0 RUNNING), DIV (clocks per sample), PKT_LEN (samples per packet = one DMA buffer), PHASE_INC (f = rate x PHASE_INC / 2^32), SAMPLES, DROPPED, PACKETS, ID `0x4D5A0501`.
- Sample word: bits 31..16 sequence number (+1 per sample, dropped ones included), bits 15..0 signed value (256-point sine table, or sawtooth).
- Like a real ADC it never waits: a sample due while the stream is not ready is dropped and counted.
- EN 0 -> 1 clears the counters and the sequence number. FLUSH stops the generator at once and holds the FIFO in reset.
- `tb_adcsim.v` passes in Icarus (packet length, values, drops when the DMA stalls, stop, FLUSH, DIV 1).

Files (all in `microzed/`):
- `fpga/05-adc-stream/`: `adcsim.v`, `tb_adcsim.v`, `build.tcl` (output `~/vivado/mz05_adc/mz05_adc.bin`).
- `modules/adcstream/adcstream.c`: owns the adcsim registers and is a DMA engine client for the S2MM channel. Ring of `nbuf` buffers (module parameter, default 8, each 256 KiB from CMA). `/dev/adcstream` (misc device, one reader): `read()` returns blocks of raw words, sleeps until one is ready, 0 at the end of the stream; `poll()`, `O_NONBLOCK`. sysfs in `/sys/bus/platform/devices/43c20000.adcstream/`: `enable`, `rate` (Sa/s), `freq`, `mode` (`sine`/`saw`), `block_samples` (64..65536, only while stopped), `blocks`, `overruns`, `pl_dropped`, `status`, `snapshot`. Starts at load: 1 MSa/s, 1 kHz sine, 4096-sample blocks. Also zero copy: `mmap()` + `ioctl` (see "mmap follow-up" below); interface in `modules/adcstream/adcstream.h`.
- `apps/adcread.c`: reads the stream, checks the sequence numbers, prints rate/min/max/mean/lost per second, `-t` seconds, `-c file.csv`, `-q` summary only, `-m` mmap (zero copy), `-s` skim (no min/max/mean).
- `overlay/usr/bin/adcstream-bench [seconds] [block_samples]`: one line per rate (100 kSa/s .. 100 MSa/s).
- `overlay/srv/www/scope.html` + `cgi-bin/adc`: live SVG chart (fetches JSON twice a second, no libraries) with rate/freq/mode/start/stop; the status page has a summary line and a link.
- `board/post-image-dt.sh`: `adcstream@43c20000 { compatible = "microzed,adcstream"; reg; clocks = <&clkc 15>; fpga-mgr; dmas = <&axi_dma 1>; dma-names = "rx"; }`.
- `overlay/etc/init.d/S40pl`: for ID `0x4d5a05xx` loads xilinx_dma + adcstream (never adcstream with another design: nothing answers at 0x43C20000).
- `Makefile`: `FPGA_BIN` prefers `mz05_adc.bin`, then 04, then 03; builds/installs `adcread` and `adcstream.ko`.

How the driver keeps the DMA busy:
- **xilinx_dma in simple mode handles only one queued buffer correctly.** `start_transfer()` programs the first pending descriptor but moves the whole pending list to "active", and the next interrupt marks every active descriptor done. So the driver keeps exactly **two** buffers with the DMA: one filling, one queued. The DMA's interrupt handler starts the queued one at once; our completion callback (in xilinx_dma's tasklet) queues the next. `dma_async_issue_pending()` right after every submit, so the queue never holds two.
- Each finished block goes to the reader's "full" queue (or straight back to "free" if `/dev/adcstream` is closed). If no free buffer is left, the oldest full block goes back to the DMA: an **overrun** (whole block lost). The block being copied by `read()` is in neither queue, so nothing can take it mid-copy.
- PL drops (FIFO full, DMA not keeping up) and overruns (reader too slow) are counted separately; the sequence numbers show both.
- Snapshot for the web page: the sysfs read sets a flag, the next callback copies 256 evenly spaced samples, the reader waits up to 1 s.
- Prep/submit/issue from the callback are fine in atomic context (xilinx_dma uses `GFP_NOWAIT` and a preallocated segment pool).

**Stop bug found on the board (fixed):** `echo 0 > enable` hung forever in `dmaengine_terminate_sync()`. The PL was stopped first, so the queued S2MM buffer sat waiting for data and the channel never halted. xilinx_dma's "1 s" halt timeout is `readl_poll_timeout_atomic(..., delay 0, 1000000)`, and in 6.12 that macro counts only 1 ns per loop when the delay is 0: about 10^15 register reads, years. One CPU spun, SSH still worked on the other, no "Cannot stop channel" message. Recovery: copy the new `.ko`, `sync; reboot -f` (a normal reboot would hang in `S40pl stop` -> rmmod). Fix: stop resubmitting, set DIV = 1 so the generator fills the (at most two) buffers the DMA holds in about 1 ms, wait for `inflight == 0`, then FLUSH the PL and terminate an idle channel.

Results (board 2, 2026-11-19):
- `adcread -t 300` at 1 MSa/s: `samples=300003328 lost=0 overruns=0 pl_dropped=0` (73,243 blocks of 4096). The rate reads 999,999 because the PL clock is 99,999,999 Hz.
- `adcstream-bench 10` (4096-sample blocks): no loss up to 2 MSa/s; overruns from 5 MSa/s (ring slack only ~5 ms); ceiling ~17.9 MSa/s; PL drops 0 everywhere.
- `adcstream-bench 10 65536` (256 KiB blocks):

| Rate | Measured Sa/s | MB/s | Overruns | PL dropped | CPU % |
|---|---|---|---|---|---|
| 1 M | 1.00 M | 4.0 | 0 | 0 | 2 |
| 5 M | 5.00 M | 20.0 | 0 | 0 | 14 |
| 10 M | 10.0 M | 40.0 | 0 | 0 | 29 |
| 14.3 M | 14.3 M | 57.1 | 0 | 0 | 42 |
| 20 M | 17.1 M | 68.4 | 436 | 0 | 50 |
| 50 M | 17.0 M | 68.2 | 5025 | 0 | 50 |
| 100 M | 16.9 M | 67.4 | 12538 | 9.4 M | 50 |

- **Limit 1, the reader: ~17 MSa/s = 68 MB/s**, one core at 100% (`copy_to_user()` from uncached coherent memory + adcread's per-sample loop). Highest lossless rate: 14.3 MSa/s (DIV 7).
- **Limit 2, the hardware: ~98 MSa/s = 393 MB/s** (32 bits x 100 MHz, as in project 04). Only at DIV 1 does `pl_dropped` rise.
- Bigger blocks fixed 5..14 MSa/s and lowered CPU: the losses there were slack (latency), not throughput.
- With 65536-sample blocks `lost` reads 0 even with overruns: the 16-bit sequence number wraps after exactly one block. Use `overruns`, or another block size.
- The live chart works (`http://10.0.0.134/scope.html`).
- Open: `status` once showed `pl_dropped=548` after the first bench (stream had run since boot at changing rates); not seen since. Watch it.

mmap follow-up (2026-11-19):
- What mmap is: the driver maps the DMA buffers into the program (`dma_mmap_coherent()`, read-only, one `mmap()` per buffer at offset `i * 256 KiB`), so data is never copied. Instead of `read()`, the program takes a block number with `ioctl(ADCSTREAM_IOC_GET)` and gives it back with `ADCSTREAM_IOC_PUT`. A held block is in no queue (bitmask `held`), so neither the DMA nor an overrun can reuse it; at most `nbuf - 3` held. `ADCSTREAM_IOC_INFO` gives nbuf, buffer size, block size. Same pattern as V4L2 video capture.
- `adcstream-bench [seconds] [block_samples] [adcread options]` passes options to adcread. Use 32768-sample blocks: with 65536 a whole lost block wraps the 16-bit sequence number and does not show in `lost` (two lost 32768 blocks in a row don't either; `overruns` is the true count).
- Bug on the way: `-m` first checked the unused `read()` buffer (constant min/max, `lost` ~65535 per sample). Test every mode on the board, not only the one the fake-data test covers.

Bench, 32768-sample blocks, 10 s per rate:

| adcread mode | No loss up to | Ceiling | CPU at 50 MSa/s |
|---|---|---|---|
| `read()` + check every sample (old loop) | 14.3 MSa/s | ~17 MSa/s (68 MB/s) | 49% |
| `read()` only (`-s`) | 50 MSa/s | ~64 MSa/s (255 MB/s) | 40% |
| mmap + check every sample (`-m`) | 5 MSa/s | ~6.5 MSa/s (26 MB/s) | 50% |
| mmap only (`-m -s`) | 50 MSa/s | 98 MSa/s (392 MB/s, DMA limit) | 1% (3% at 100 M) |
| `read()` + check, new loop | **25 MSa/s** | ~26.6 MSa/s (106 MB/s) | 50% |

- The copy was not the main cost: the per-sample loop was.
- **Uncached memory is slow to read word by word**: every load goes to DDR. The kernel's `copy_to_user()` reads it in bursts and the loop then runs on cached memory, so `read()` + check beats mmap + check by 4x. mmap pays off only when the program does not touch every sample (or with cached buffers + `dma_sync_single_for_cpu()`).
- With mmap and no per-sample work the software is no longer a limit: 98 MSa/s with 3% CPU, every loss a PL drop (the DMA's 32 bit x 100 MHz).
- Kept the simple option: `read()` with a cheaper loop. Sequence check once per block from the two end samples (exact, numbers only move forward), integer min/max/sum without branches (int32 block sum cannot overflow for <= 65536 samples), CSV in its own loop. Ceiling 17 -> 26.6 MSa/s, no loss 14.3 -> 25 MSa/s, about a third less CPU per sample. The limit is now one core at 100% (adcread is single-threaded).

### Buildroot packages (project 06, in progress 2026-11-19)

```
microzed/br2-external/            BR2_EXTERNAL tree, name MICROZED
  external.desc, Config.in, external.mk
  package/microzed-modules/       kernel-module + generic-package, source = microzed/modules (local)
  package/microzed-apps/          generic-package, source = microzed/apps (local)
```

- `microzed-modules`: builds `myled-irq axiregs pltimer dmaloop adcstream` (`MODULE_SUBDIRS`) against the Buildroot kernel, installs them with `INSTALL_MOD_DIR=microzed` in `/lib/modules/<version>/microzed/`. Buildroot's `depmod` runs in target-finalize (after all packages, before the overlay), so `modprobe` finds them. Also installs `adcstream.h` in staging (`usr/include/microzed/`), like a library header.
- `microzed-apps`: `hello`, `blink` (`-lgpiod`), `adcread` (`-I$(STAGING_DIR)/usr/include/microzed`), into `/usr/bin`. Depends on libgpiod and microzed-modules; `leddaemon` is no longer built.
- `S30myled` and `S40pl` use `modprobe <name>` instead of `insmod <path>`. S40pl still loads the PL drivers only after the bitstream (no automatic loading from the device tree, on purpose).
- `microzed/Makefile`: `make` = `make microzed-modules-rebuild microzed-apps-rebuild` in `~/br-microzed` (`-rebuild` re-copies local sources; a plain `make` does not notice changes). `make push` runs Buildroot's host depmod on `target/` and copies the `.ko` files + `modules.*` to the board (BusyBox has `modprobe` but no `depmod`). `make overlay` removes the old `/lib/modules/{myled,axiregs,...}` folders and old program copies from `~/mz-overlay` (the overlay is copied last, so a stale `usr/bin/adcread` there would replace the package's).
- Checked in the cloud session against a clone of Buildroot 2026.02.x: `zynq_microzed_defconfig` + the external tree parses, `microzed-apps` selects modules + libgpiod (1.6.5), dependencies and hooks are as expected. Not tested: the actual build (no ARM toolchain/kernel there).

### Repo workflow (`~/microzed-linux/microzed`)

All source files now live in the git repo, in `microzed/` (see `microzed/README.md`). Claude writes files there; the laptop gets them with git.

- Laptop clone: `~/microzed-linux` (private repo: git asks for the GitHub username and a personal access token). Update with `cd ~/microzed-linux && git pull`.
- Repo: `Fuzzyvolt/microzed-linux` (renamed from `Trunk`), default branch `main` (renamed from `claude/github-setup-i4kllz`). Each session works on its own branch; merge its pull request into `main` when done, so the next session sees the work.
- `cd ~/microzed-linux/microzed`, then:
  - `make`: cross-compile the programs into `build/`.
  - `make`: also builds the `myled_irq` kernel module.
  - `make push`: unload the driver (and remove leddaemon if still there), copy the module, programs and scripts to the running board over SSH, load the driver again. No re-flash needed.
  - `make overlay`: copy programs and overlay files into `~/mz-overlay`.
  - `make flash`: `make overlay`, rebuild Buildroot, then `flash-sd.sh` (checks the card is removable and not the PC's disk, asks for `yes`, unmounts, `dd`, sync, eject). `make flash SD=/dev/sdc` for another device.
- Use `make push` for program/script changes, `make flash` for Buildroot changes (packages, kernel, BusyBox) or to make changes permanent.
- Kernel modules: `microzed/modules/<name>/`, built with their own Makefile (`make`, `make push`).

### SSH key login

- Key made on the laptop with `ssh-keygen -t ed25519`, copied with `ssh-copy-id root@10.0.0.134` (run on the laptop, not on the board).
- To keep it after re-flashing, it is in the overlay: `~/mz-overlay/root/.ssh/authorized_keys` (folder 700, file 600; Dropbear ignores the key if permissions are looser).
- `~/.ssh/id_ed25519` (no `.pub`) is the private key: keep it on the laptop only.

### LED daemon (`microzed/apps/leddaemon.c`, retired)

Replaced by the `myled_irq` kernel driver on 2026-11-15. Its init script is kept as `microzed/apps/leddaemon.init`. What it did:

- Was started at boot by `/etc/init.d/S99leddaemon` (BusyBox `start-stop-daemon`, pid file `/var/run/leddaemon.pid`).
- Owns LED D3 (output) and PB1 (both-edge events, so it sleeps until the button interrupt fires). Debounce: after an edge, wait until the line is quiet for 30 ms, then read the settled value; a 0 -> 1 change toggles the LED.
- Commands through a FIFO: `echo on|off|toggle > /tmp/led.fifo`. The FIFO is opened read-write, so it never reports end-of-file when a writer closes it.
- Writes the state to `/tmp/led.state` (temp file + rename, so readers never see half a file). Logs to syslog: `logread | grep leddaemon`. Foreground debug: stop the service, run `leddaemon -f`.
- The web page's `control` CGI only accepts POST with exact values (`led_on`, `led_off`, `led_toggle`, `poweroff`) and redirects back with `303 See Other`. Power off asks for confirmation in the browser. It used to send commands to the daemon's FIFO (only if the daemon was running: a FIFO with no reader blocks forever); it now writes `1`/`0`/`t` to `/dev/myled`.
- While the daemon runs, `blink` and `gpioset` on lines 47/51 fail with "Device or resource busy" (only one program can own a line). Stop the daemon first.
- Daemon vs CGI: an S99 daemon is started once by init and runs until shutdown (keeps state, reacts to hardware); a CGI script is started by httpd for each request and exits after printing the page. Real devices combine them: the daemon owns the hardware, the CGI talks to it.

### Rootfs overlay (`~/mz-overlay`)

Copied into the image at the end of every `make` (no package rebuild needed, but `make` must run before flashing).

| File in overlay | Purpose |
|---|---|
| `etc/network/interfaces` | lo + eth0 DHCP, with `pre-up ip link set eth0 address 02:0a:35:00:00:01` (fixed MAC) |
| `etc/dropbear/*` | SSH host keys copied from the board, so the key stays the same after re-flashing. Private: never commit |
| `usr/bin/hello`, `usr/bin/blink` | cross-compiled test programs |
| `srv/www/cgi-bin/status` | status page CGI script (shell + awk) |
| `srv/www/cgi-bin/index.cgi` | redirects `/` to `/cgi-bin/status` (see BusyBox bug below) |
| `etc/init.d/S90httpd` | starts `httpd -h /srv/www -p 80` at boot |

The MAC is set from Linux rather than U-Boot because the U-Boot environment lives on the SD boot partition and is erased by every `dd`. Both boards use this card, so they have the same MAC and IP: never put both on the network at once (change the last MAC byte on one of them if needed).

### Cross-compiling

- Compiler: `~/br-microzed/host/bin/arm-linux-gcc` (short name Buildroot provides; the long name is `arm-buildroot-linux-gnueabihf-gcc`).
- Programs live in `~/mz-apps` (`hello.c`, `blink.c`).
- Build: `arm-linux-gcc -Wall -O2 -o blink blink.c -lgpiod`. Buildroot's compiler finds the headers and libraries in its sysroot automatically.
- Check with `file blink` (should say `ELF 32-bit LSB executable, ARM`), copy with `scp -O`.
- `blink.c` uses the libgpiod v1 C API: `gpiod_chip_open_by_name`, `gpiod_chip_get_line`, `gpiod_line_request_output/input`, `gpiod_line_set_value/get_value`. It blinks LED D3 until PB1 is pressed.

### XADC (temperature and voltages)

- The kernel's XADC driver shows up as `/sys/bus/iio/devices/iio:device0` (`name` = `xadc`).
- Temperature in degrees C = `(in_temp0_raw + in_temp0_offset) * in_temp0_scale / 1000`.
- Voltages in V = `raw * scale / 1000`. Channels: vccint (1.0 V, PL core), vccaux (1.8 V), vccbram (1.0 V), vccpint (1.0 V, PS core), vccpaux (1.8 V), vccoddr (1.5 V, DDR3), vrefp (1.25 V), vrefn (0 V).

### BusyBox httpd

- Not in the default Buildroot BusyBox. Enabled with a config fragment `~/mz-busybox.fragment` (`CONFIG_HTTPD=y`, `CONFIG_FEATURE_HTTPD_CGI=y`), set in menuconfig under Target packages -> BusyBox -> "Additional BusyBox configuration fragment files". Applied with `make busybox-dirclean && make`.
- **BusyBox bug:** with CGI on but `FEATURE_HTTPD_BASIC_AUTH` off, a request for `/` returns 404 even though `index.html` exists (`/index.html` works). httpd appends `index.html`, truncates it for the auth check, and only restores it when basic auth is compiled in. Found by reading the BusyBox source (master branch).
- Workaround used: no `index.html`; `cgi-bin/index.cgi` sends a 302 redirect to `/cgi-bin/status` (httpd only falls back to `index.cgi` when `index.html` does not exist).
- Alternative fix: add `CONFIG_FEATURE_HTTPD_BASIC_AUTH=y` to the fragment, then `make busybox-dirclean && make`.
- `curl` is not on the board; use `wget -O - http://127.0.0.1/...` there.

### Why libgpiod2 was dropped (solved)

- `BR2_PACKAGE_LIBGPIOD2` was missing from `.config`, and `BR2_TOOLCHAIN_HEADERS_AT_LEAST_5_10` was not set. libgpiod2 needs kernel headers >= 5.10, so menuconfig hides it and silently drops it.
- Fix: use **libgpiod v1** (`BR2_PACKAGE_LIBGPIOD` + "install tools"). It works with older headers. Moving to libgpiod2 later would need newer toolchain headers and a full `make clean && make`.
- v1 tools include `gpiofind`, which v2 does not have. The syntax also differs from v2:

```bash
gpiodetect
gpioset --mode=wait gpiochip0 47=1   # LED D3 on; press Enter to release
gpioget gpiochip0 51                 # button PB1: 0 released, 1 pressed
gpiomon gpiochip0 51                 # watch button events, Ctrl-C to stop
```

### Why DHCP was missing from the image (solved)

- `BR2_SYSTEM_DHCP="eth0"` was already set in `.config`, but `/etc/network/interfaces` on the board only had `lo`.
- Cause: the file is generated by the `ifupdown-scripts` package, which had been built before the option was set. A plain `make` does not rebuild a package when an option it uses changes.
- Fix: `make ifupdown-scripts-rebuild && make`, then re-flash.
- Manual workaround on a running board: `ip link set eth0 up` then `udhcpc -i eth0`.

### Ethernet PHY not detected at power-up (open, workaround found)

Symptoms on a bad boot:
- U-Boot: `Net: Could not get PHY for eth0: addr 0` / `No ethernet found.`
- Linux: `macb e000b000.ethernet eth0: Could not attach PHY (-19)`, `mdio_bus e000b000.ethernet-ffffffff: MDIO device at address 0 is missing.`, and `ls /sys/bus/mdio_bus/devices/` is empty. `ip link set eth0 up` fails with `SIOCSIFFLAGS: No such device`.
- Good boot: U-Boot shows `ZYNQ GEM: e000b000, mdio bus e000b000, phyaddr 0, interface rgmii-id` and `eth0: ethernet@e000b000`; Linux shows `e000b000.ethernet-ffffffff:00`.

What was tested:
- Not a power-supply problem: it failed on both laptop USB power and a 5 V wall supply.
- Once the PHY is missing, it stays missing for that power-on. Repeated `dhcp` in U-Boot keeps failing, and `reboot` from Linux does not bring it back.
- Pressing SW2 or power-cycling sometimes brings it back. The results were inconsistent: most cold boots failed.
- The factory QSPI image (JP2 1-2) once showed `xemacps ... link up (1000/FULL)`. Its fixed IP is `192.168.1.10`; `udhcpc` got no reply and RX packets stayed 0 (probably the old software's RGMII timing, not tested further).
- The RJ45 link LEDs stay on even when the PHY is "missing". The PHY makes the cable link by itself; only its MDIO management interface stops answering.

**Workaround that sometimes works:** power up **without the Ethernet cable plugged in**, then plug it in after boot. This helped at first, but later it also failed often.

From the Avnet user guide (section 2.7 and 2.10.3): the PHY reset `ETH_RST_N` is **not** on an MIO pin. It is driven by PG_MODULE (the 3.3 V power-good signal, which is the board's power-on reset) through level translator U8. So:
- Software cannot reset the PHY, and a `reset-gpios` device tree fix is not possible.
- SW2 (PS_SRST#) does not reset the PHY either. Earlier "SW2 fixed it" results were probably luck.
- Only a full power cycle resets the PHY. If it comes out of the power-on reset in a bad state, it stays that way.

Conclusion so far: a board-level power-up problem on board 1 (reset timing vs. the 25 MHz oscillator, the U8 translator, or a weak PHY). Not yet done: count the factory image's cold-boot success rate, and run 5 cold boots with the cable plugged in on board 2. If board 2 is always fine, board 1 is faulty (contact Avnet: "88E1512 not responding on MDIO after power-on, intermittent"). Fallback option: a USB-Ethernet adapter (ASIX AX88179 / AX88772) on the USB host port.

### Kernel modules (`microzed/modules/`)

Each module has its own folder and Makefile. The Makefile calls the kernel's own build system (Kbuild) on the tree Buildroot built (`~/br-microzed/build/linux-*`) with `ARCH=arm CROSS_COMPILE=~/br-microzed/host/bin/arm-linux-`, so the module matches the running kernel exactly (its `vermagic` must match, or `insmod` gives "invalid module format"). `make push` copies the `.ko` to `/root/` on the board.

**hello** (works): `module_init`/`module_exit`, `pr_info` with a `pr_fmt` prefix, a module parameter (`insmod hello.ko who=world`, readable in `/sys/module/hello/parameters/who`), `jiffies`/`HZ` timing. `modinfo hello.ko` shows the license, description, parameter and vermagic. Changing init to `return -ENODEV;` makes the load fail, which is how a driver refuses missing hardware (like macb's "Could not attach PHY (-19)", -19 = -ENODEV).

**myled** character driver (works): `/dev/myled` controls LED D3.
- `echo 1|0|t > /dev/myled` sets on/off/toggle, `cat /dev/myled` prints the state, anything else gives "Invalid argument" (`-EINVAL`).
- Init order: `alloc_chrdev_region` (major:minor, listed in `/proc/devices`) -> `class_create` (`/sys/class/myled`) -> `device_create` (devtmpfs creates `/dev/myled`) -> GPIO lookup table + `gpiod_get` -> `cdev_add` last, so nobody can open the device before the GPIO is ready. The error path (`goto err_...`) and the exit function undo the steps in reverse order.
- User data is copied with `get_user` / `simple_read_from_buffer` (never dereference a user pointer). A mutex protects the LED state.
- The LED pin was hard-coded with a `gpiod_lookup_table` (chip label `zynq_gpio`, line 47). The next step replaces this with a device tree node.
- Loading it while leddaemon runs fails with "Device or resource busy" (`-16`, -EBUSY): the kernel allows one owner per GPIO line. Stop leddaemon first, start it again after `rmmod myled`.
- `class_create()` lost its first argument (`THIS_MODULE`) in kernel 6.4, handled with a `LINUX_VERSION_CODE` check.

**myled-dt** device tree + platform driver (works): same `/dev/myled`, but the hardware comes from a device tree node.
- `microzed/board/post-image-dt.sh` adds this node to the built `.dtb` with `fdtput` (from `device-tree-compiler`), giving the GPIO controller a `phandle` if it has none:
  ```
  myled { compatible = "microzed,myled"; label = "d3"; led-gpios = <&gpio0 47 0>; };
  ```
  Editing the compiled `.dtb` avoids depending on the kernel's `.dts` file location and keeps the `.dtb` name the boot setup expects.
- Buildroot setting: `BR2_ROOTFS_POST_IMAGE_SCRIPT="/home/fabio/microzed-linux/microzed/board/post-image-dt.sh board/zynq/post-image.sh"`. **Order matters**: our script must run before `board/zynq/post-image.sh`, which runs genimage to make `sdcard.img`.
- Check before flashing: `fdtget ~/br-microzed/images/*.dtb /myled compatible`. On the board: `ls /proc/device-tree/myled/`; the platform device exists even before the driver loads (`/sys/bus/platform/devices/`).
- The driver only registers itself in init; the kernel calls `probe()` for each node whose `compatible` matches `of_match_table`. `probe()` reads `label` (`device_property_read_string`) and `led-gpios` (`devm_gpiod_get(dev, "led", ...)`), then creates `/dev/myled`. `devm_*` resources are freed automatically on unbind. Binding shows in `/sys/bus/platform/drivers/myled/`.
- `platform_driver.remove` returns void from kernel 6.11 (int before), handled with `LINUX_VERSION_CODE`.

**myled-irq** button interrupt driver (works): adds PB1 to the same node (`button-gpios = <&gpio0 51 0>`, written by `post-image-dt.sh`, which now updates an existing node instead of skipping it).
- `/dev/mybutton`: `read()` sleeps until PB1 is pressed, then returns `press N`. Supports `poll()`/`select()` and `O_NONBLOCK` (`-EAGAIN`). Each `open()` gets its own "last press seen" counter, so several readers each get every press. `/dev/myled` works as before. Both use one major number (minor 0 = myled, minor 1 = mybutton).
- Interrupt: `gpiod_to_irq()` gives the button's IRQ; `devm_request_threaded_irq(..., NULL, thread_fn, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT, ...)`. The threaded handler (bottom half) may sleep: it waits 20 ms (`msleep`), reads the settled value, and on a 0 -> 1 change toggles the LED, increments an atomic press counter and calls `wake_up_interruptible()`. A plain top-half handler runs in interrupt context and may not sleep. `IRQF_ONESHOT` keeps the line masked until the thread returns.
- Readers sleep in `wait_event_interruptible()` on a wait queue, using no CPU; Ctrl-C (a signal) ends the wait with `-ERESTARTSYS`.
- `/proc/interrupts` shows the `myled-button` line: about two interrupts per press (press + release, plus bounce), while the debounced press count rises by one.
- Test: `cat /dev/mybutton`, or `while read p < /dev/mybutton; do echo "Got $p"; done`.
- This is the pattern a DMA or ADC driver uses: the hardware interrupts when data is ready, the handler wakes the reader, `read()` returns the data.

**Driver loaded at boot** (works):
- `make overlay` / `make flash` install the module as `/lib/modules/myled/myled_irq.ko`; `/etc/init.d/S30myled` loads it with `insmod` (start/stop/restart by hand). The boot console shows `Loading myled_irq: OK`; `lsmod` lists it.
- `insmod` with a full path instead of `modprobe`: `modprobe` needs the index built by `depmod`, and Buildroot runs `depmod` before copying the overlay, so it never sees our module. The proper long-term fix is a Buildroot package (`BR2_EXTERNAL`, kernel-module infrastructure), which also rebuilds the module with the kernel.
- After a kernel change in Buildroot, rebuild the module (`make`), or it fails to load at boot with "invalid module format".
- The web page now writes `1`/`0`/`t` to `/dev/myled` and reads the LED state from it (`[ -c /dev/myled ]` checks the driver is loaded). Full chain: browser -> httpd -> CGI -> `/dev/myled` -> our driver -> GPIO -> LED.

### Wi-Fi disruption while the board is on the network (open, watching)

- Symptom: while the board is connected to the Vidéotron router, a video call on another computer (on Wi-Fi) stops. Not an IP conflict (the other computer has a different IP), and only one `Link is Up` in `dmesg` (no link flapping).
- Suspect: Ethernet **pause frames**. `ethtool eth0` shows the board advertises `Transmit-only` pause and the router `Symmetric Receive-only`, so the board may send pause frames and the router obeys them (boot log: `flow control tx`). On home routers this can stall the shared switch, including Wi-Fi traffic.
- `ethtool -a eth0` gives "Operation not supported" (the macb driver has no pause settings), so pause is turned off through the PHY advertisement instead: `ethtool -s eth0 advertise 0x02f` (10/100 half+full and 1000 full, no Pause/Asym_Pause bits). Then `dmesg | tail` should show `flow control off`.
- Not applied yet: `ethtool -S eth0 | grep -i pause` showed no pause frames so far. Next time the problem happens, check whether `tx_pause_frames` climbs; if so, apply the advertise fix and make it permanent with a `post-up` line in `overlay/etc/network/interfaces`.
- Other test: stop the board at the U-Boot prompt during a call. If the call still drops, the PHY itself causes it (hardware).

### Second board and SD card corruption

- The same SD card boots on a second MicroZed (same PS for 7010/7020; set the boot jumpers to SD first).
- On the first try it panicked: `EXT4-fs (mmcblk0p2): group descriptors corrupted!` / `VFS: Unable to mount root fs on unknown-block(179,2)` (179,2 = mmcblk0p2, the rootfs).
- Cause: power was cut while the board was writing to the card (easy to do with all the PHY power-cycling). Fixed by re-flashing (or `sudo fsck.ext4 -fy /dev/sdb2`).
- **Always run `poweroff` and wait for `reboot: System halted` before unplugging.**

## Next steps

The next projects are written as self-contained briefs in `microzed/projects/` (index: `microzed/projects/README.md`), one per session. Start a new session with, for example: "Read `buildroot-microzed-project.md` and `microzed/projects/02-axi-registers.md`, then let's do it." End each session with "update the md".

1. ~~`01-vivado-setup.md`: install Vivado, first bitstream.~~ Done 2026-11-15.
2. ~~`02-axi-registers.md`: custom AXI register block, load the bitstream from Linux, driver.~~ Done 2026-11-17.
3. ~~`03-pl-timer-interrupt.md`: PL timer interrupt, blocking read.~~ Done 2026-11-17.
4. ~~`04-axi-dma-loopback.md`: AXI DMA loopback with the DMA engine framework.~~ Done 2026-11-18 (393 MB/s at 1 MiB).
5. ~~`05-dma-data-stream.md`: simulated ADC stream to a live web chart.~~ Done 2026-11-19 (zero loss at 1 MSa/s for 5 min, up to 14 MSa/s).
6. `06-buildroot-package.md`: build our modules and programs as Buildroot packages, `modprobe`. Files written 2026-11-19; test on the laptop and board.
7. `07-iio-adc-driver.md`: the ADC stream as a standard IIO device (libiio tools, `iio_readdev`).
8. `08-network-stream.md`: every sample to the laptop over TCP, then UDP; Python receiver that checks, records, plots and runs an FFT. The web page stays as the status view; this is the real data channel.

Next session: `06-buildroot-package.md`, then 07, then 08. Later: a real sensor once a MicroHeader breakout is available (SPI ADC or pressure sensor sampled by the PL in place of adcsim, same driver); cached DMA buffers, scatter-gather, FIR filter in the PL, QSPI boot, read-only rootfs, watchdog, FreeRTOS/AMP (see "Longer roadmap").

Not yet tried in project 05 (optional):
- ~~`mmap()` of the ring.~~ Done, see "mmap follow-up". Still open: cached buffers + `dma_sync_single_for_cpu()` (fast per-sample reads with zero copy); a second thread so copy and processing use both cores; NEON for min/max/sum.
- Scatter-gather mode (needs `c_include_sg 1` and the M_AXI_SG port on HP0): many buffers queued in hardware, no two-in-flight limit. Note: cyclic mode in xilinx_dma needs SG; in simple mode it does not work.
- A bigger `nbuf` (module parameter) vs. overruns at 15..17 MSa/s.
- `adcread`: count overruns into `lost` so 65536-sample blocks report honestly.

Not yet tried in project 04 (optional): re-run the 4 MiB size (`echo 4194304 > size; echo 10 > run; cat result`); 64-bit stream/memory width (ceiling 800 MB/s, HP0 is already 64-bit; change `c_m_axi_*_data_width`, `c_*_tdata_width` and DT `xlnx,datawidth = <64>`); `dma_map_single()` + cached buffers instead of coherent ones, to compare CPU fill/compare time.

Not yet tried in project 03 (optional): stress test at `period_us` 50 (`missed`, CPU load in `top`), and user-space jitter (`head -20 /dev/pltimer | awk ...`, compare with the driver's ±3.5 µs).

Still open on the side: watch the pause counters during the next Wi-Fi disruption; PHY test on board 2 (5 cold boots with the cable plugged in).

## Longer roadmap

1. Linux on hardware: GPIO, SSH, U-Boot environment, QSPI boot.
2. Kernel: "hello" module, character driver, device tree edits.
3. FPGA: Vivado (free edition supports the 7010; tens of GB download), AXI GPIO/PWM in the PL, controlled from Linux (UIO, then a kernel driver), FPGA manager.
4. AXI DMA: stream data from the PL into DDR and read it from Linux.
5. Bare-metal Cortex-A9 and FreeRTOS, then Linux + FreeRTOS on separate cores (OpenAMP).
6. Small instrument: SPI ADC or pressure sensor, DMA, Linux, streaming over Ethernet; optionally Modbus RTU over RS-485.

Note: most PL (FPGA) pins only reach the MicroHeaders on the underside, so PL I/O needs a carrier card or breakout. The LED, button and Pmod J5 are on the PS side.

## Lessons learned / gotchas

- Run `make menuconfig` from the folder of the build you mean to change (`~/br-microzed` for the board). Afterwards, check with `grep` on that folder's `.config`.
- After `make olddefconfig` or menuconfig, check that options you added are still `=y`. Unmet dependencies drop them silently.
- Before `dd`, check the SD card name with `lsblk` and unmount its partitions. Write to the whole disk (`/dev/sdb`), not a partition.
- Boot jumpers are read only at power-on.
- `picocom` "Permission denied" means the `dialout` group isn't active yet (log out/in, or `newgrp dialout`). "No such file" means the board isn't connected or the port has another name (check `lsusb`, `ls /dev/ttyUSB*`, `sudo dmesg | tail`).
- Quit picocom: `Ctrl-A` then `Ctrl-X`. Quit QEMU: `Ctrl-A` then `X`.
- Save and exit menuconfig: `Esc Esc` until it asks to save, then Yes.
- Rebuild rules: new package means `make`; one changed package means `make <pkg>-rebuild && make`; toolchain or arch change means `make clean && make`.
- A plain `make` does not rebuild a package when a menuconfig option it uses changes (example: `BR2_SYSTEM_DHCP` and `ifupdown-scripts`). Rebuild that package. If unsure which package uses the option, `make clean && make`.
- Before re-flashing, check the result in `~/br-microzed/target/` (for example `ls target/usr/bin | grep gpio`, `cat target/etc/network/interfaces`).
- A Buildroot package can be hidden in menuconfig because of a toolchain dependency (for example libgpiod2 needs headers >= 5.10). Search with `/` to see its "Depends on" line.
- Re-flashing creates a new Dropbear host key. Clear the old one on the PC with `ssh-keygen -R <ip>`.
- Find the board on the network without serial: `nmap -p 22 --open 10.0.0.0/24`.
- Power up the MicroZed without the Ethernet cable, then plug it in (see the PHY issue above).
- The overlay is copied during `make`. A file added to `~/mz-overlay` after the last `make` is not in `sdcard.img` until you run `make` again.
- BusyBox options are changed with a config fragment, then `make busybox-dirclean && make` (a plain `make` does not reconfigure an already-built BusyBox).
- Long `cat << EOF` pastes through KDE Connect can get cut off. For long files, paste into `nano` (Ctrl+Shift+V, save Ctrl+O, exit Ctrl+X) and check with `sh -n` and `tail`.
- Run `poweroff` before unplugging, or the ext4 rootfs can get corrupted.
- End an SSH session with `exit` or Ctrl-D; force-close a frozen one with Enter, `~`, `.`.
- Board shell errors look like `-sh: <cmd>: not found`. If a laptop command (`ssh-copy-id`, `curl`, `make`) gives that, you are in the board's SSH session: `exit` first. Prompt `#` = board, `fabio@fabio-UX430UAR:~$` = laptop.
- A background command in a CGI script must redirect its input and output (`</dev/null >/dev/null 2>&1 &`), or httpd waits for it and the page hangs.
- Copying over a running program fails with "Text file busy": stop it first.
- The overlay only adds files. To remove something from the image (like the old `S99leddaemon`), delete it from `~/mz-overlay` **and** from `~/br-microzed/target/` (`make overlay` does this for `S99leddaemon`).
- `poweroff` stops services, syncs and halts Linux, but the MicroZed has no software power switch: the LEDs stay on. Wait for `reboot: System halted` (serial) or for SSH/ping to stop, then unplug.
- Vivado 2026.1 on Mint 22: needs `libtinfo5` **and** `libncurses5` from Ubuntu 22.04 (`libncurses.so.5: cannot open shared object file` otherwise). Package file names change with security updates (`ubuntu0.1` -> `ubuntu0.3`): look in the pool folder if a URL gives 404.
- AMD's `installLibs.sh` ships with Windows line endings (`sudo: unable to execute ...: No such file or directory`, `head -1 | cat -A` shows `^M$`). Run a cleaned copy: `tr -d '\r' < installLibs.sh > /tmp/installLibs.sh`, then `sudo bash /tmp/installLibs.sh`.
- Vivado Tcl does not expand `~` in `set_param board.repoPaths [list ...]`; use the full path. Test with `get_board_parts *microzed*` in the Tcl console.
- `vivado: command not found` in a new terminal: run `vivado-env` (or source `settings64.sh`) first.
- **Never read a PL address (`devmem`, driver) before the bitstream is loaded**: nothing answers on the AXI bus and the CPU hangs until a power cycle. Check `cat /sys/class/fpga_manager/fpga0/state` = `operating` first. The driver checks this itself.
- The Zynq FPGA manager wants a byte-swapped `.bin`, not the `.bit` ("Invalid bitstream, could not find a sync word" otherwise). `bit2bin.tcl` makes it without Vitis.
- Write custom PL blocks as plain Verilog + module reference in the block design; simulate them first (Icarus `iverilog`, or Vivado's simulator). Much faster than a bitstream build to find AXI handshake bugs.
- A new device tree node needs `make flash` (post-image script); a new module or script alone only needs `make push`.
- A level interrupt from the PL must be cleared in the device (W1C) by the handler, followed by a read-back, or it fires forever.
- Device tree SPI number = GIC ID - 32. Zynq IRQ_F2P[0] = ID 61 = `<0 29 4>`.
- When a new bitstream replaces the old one, keep the old blocks in the new design (only one bitstream at a time) and keep their addresses, so the old drivers still work.
- A driver that reads PL registers in `probe()` must not be built into the kernel when its DT node is in the boot `.dtb` and the PL is loaded from Linux: it binds before the bitstream and hangs the CPU. Make it a module (kernel config fragment) and load it after the bitstream. Xilinx's AXI DMA driver is one (`CONFIG_XILINX_DMA=y` in `xilinx_zynq_defconfig`).
- Kernel options: Buildroot "Additional configuration fragment files" + `make linux-reconfigure && make`; check with `grep` on `~/br-microzed/build/linux-*/.config`. After a kernel rebuild, `make clean && make` our modules.
- AXI DMA "buffer length register width" defaults to 14 bits (16 KB per transfer). Set it in Vivado and in DT `xlnx,sg-length-width` to the same value.
- DMA buffers: allocate with the DMA controller's device (`chan->device->dev`), use coherent memory or map + sync (the HP ports bypass the caches); start the receive channel before the transmit one.
- The DMA's address window to DDR must cover all RAM (1 GB): CMA buffers sit near the top (~0x3F000000).
- Vivado Tcl wiring by hand (`connect_bd_intf_net`, `connect_bd_net`, `axi_interconnect` with `NUM_SI`/`NUM_MI`) is predictable and readable; connection automation is fine for a single slave but picks names and interconnect types itself.
- xilinx_dma in simple (non-SG) mode: queue at most one descriptor behind the running one, and call `dma_async_issue_pending()` after each submit. Several queued descriptors are all "completed" by one interrupt.
- Before `dmaengine_terminate_sync()` on an S2MM channel, make sure it holds no buffer (feed it data until done). A channel waiting for stream data never halts, and xilinx_dma's halt timeout never expires (`readl_poll_timeout_atomic` with delay 0 counts 1 ns per loop).
- A stuck kernel call that holds a driver lock also blocks `rmmod`, so a normal `reboot` hangs in the init scripts: `sync; reboot -f`. Replace the `.ko` first so the fixed driver loads at boot.
- A streaming source that cannot wait (ADC) needs a FIFO big enough for the CPU's worst re-arm delay, and a ring with enough slack for the reader's worst pause. Bigger blocks = more slack and fewer interrupts.
- Put a sequence number in every sample: it proves "no loss" end to end, independent of the driver's counters. Make the counter wider than the largest block, or whole-block losses hide.
- Read the driver source of the DMA engine you use (here `drivers/dma/xilinx/xilinx_dma.c`) before designing a client: the API is generic, but each driver's behaviour is not.
- Uncached (coherent) DMA memory: fine to copy in bulk, very slow to read word by word. Zero copy (mmap) only wins if the program does not read every sample from it; otherwise copy first (`read()`) or use cached buffers with explicit cache syncs.
- Measure before optimising: splitting the time with `-s`/`-m` showed the per-sample loop, not the copy, was the bottleneck.
