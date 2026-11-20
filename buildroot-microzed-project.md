# MicroZed embedded Linux: project notes

Last updated: 2026-11-20 (projects 01 to 08 done; repo cleanup)

Load this file at the start of a session to continue the project. How sessions work (commands in chat, files in `microzed/`, `git pull` on the laptop) is in `CLAUDE.md`; the repo layout is in `microzed/README.md`.

## Goal

Learn embedded Linux hands-on: build a Linux system with Buildroot and run it on real hardware (Avnet MicroZed), then kernel drivers, FPGA (PL) designs, AXI DMA and FreeRTOS. These cover gaps in real DMA, RTOS and driver experience.

The learner has little hands-on embedded Linux experience. Explain from the basics, with diagrams first.

## Current state

- **Board 2** is in use: board 1's Ethernet PHY is unreliable (see "Hardware problems"). Both boot the same SD card.
- Boots from SD; IP **10.0.0.134** (fixed MAC); `ssh root@10.0.0.134` with a key.
- At boot: `myled_irq` (LED D3 + PB1), `S40pl` programs the project 05 bitstream and loads its drivers (adcstream by default), BusyBox httpd (status page + live ADC chart), `adcserve` TCP and UDP servers.
- Projects done:
  - 01 Vivado installed, first bitstream built from a Tcl script.
  - 02 AXI register block in the PL, loaded from Linux, driver with sysfs files.
  - 03 PL timer interrupt, `/dev/pltimer`, about ±3.5 µs jitter.
  - 04 AXI DMA loopback through the DMA engine framework, 393 MB/s.
  - 05 Simulated ADC stream into a DDR ring, `/dev/adcstream`, web chart; no loss up to 25 MSa/s, zero copy reaches the DMA's 98 MSa/s.
  - 06 Modules and programs are Buildroot packages (`modprobe`).
  - 07 The same ADC as a standard IIO device (`iio_readdev`), no loss up to 25 MSa/s.
  - 08 Every sample to the laptop over TCP or UDP; over Wi-Fi TCP is lossless up to 10 MSa/s, UDP up to 5 MSa/s.
- Next: no brief for project 09 yet (see "Next steps").

## Hardware and host

| Item | Details |
|---|---|
| Build PC | Linux Mint laptop (ASUS UX430UAR), user `fabio`, x86_64 |
| Target board | Avnet MicroZed, Zynq-7010 (XC7Z010), 2x Cortex-A9 (32-bit ARMv7), 1 GiB DDR3, 32 MB QSPI (S25FL256S), microSD, Gigabit Ethernet (Marvell 88E1512) |
| Serial console | Micro-USB J2 (power + CP210x USB-UART) appears as `/dev/ttyUSB0` on the PC, 115200 8N1, using `picocom` |
| SD card | 14.4 GB microSD, shows as `/dev/sdb` on the PC (always check with `lsblk`; `/dev/sda` is the PC's own drive) |
| Reference | `microzed/docs/Avnet-MicroZed-7Z010-7Z020-EN-User-Guide.pdf` |

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

## Buildroot setup

- Host packages: `build-essential git wget cpio unzip rsync bc file libncurses-dev qemu-system-arm picocom`; user `fabio` is in the `dialout` group (for `/dev/ttyUSB0`).
- Buildroot source: `~/buildroot`, from `https://gitlab.com/buildroot.org/buildroot.git`, LTS branch `2026.02.x`. Its `dl/` folder is the download cache.
- **MicroZed build: `~/br-microzed`** (out-of-tree, made with `make O=$HOME/br-microzed zynq_microzed_defconfig`). Images in `~/br-microzed/images/` (`boot.bin`, `u-boot.img`, kernel, `.dtb`, `sdcard.img`). Config backup: `~/mz-overlay-microzed.config`.
- QEMU (dropped 2026-11-14): `~/buildroot/output` holds an old AArch64 QEMU build, unused. QEMU could come back for kernel debugging with gdb (`-M xilinx-zynq-a9`).
- Each build has its own `.config`: run `make menuconfig` in `~/br-microzed`, not in `~/buildroot` (it happened once).

What `~/br-microzed/.config` has on top of `zynq_microzed_defconfig`:
- `BR2_EXTERNAL` = `/home/fabio/microzed-linux/microzed/br2-external`, with `BR2_PACKAGE_MICROZED_APPS=y` and `BR2_PACKAGE_MICROZED_MODULES=y` (project 06).
- Kernel config fragment `/home/fabio/microzed-linux/microzed/configs/linux.fragment` (`CONFIG_XILINX_DMA=m`, IIO DMA buffer modules).
- Post-image scripts: `/home/fabio/microzed-linux/microzed/board/post-image-dt.sh board/zynq/post-image.sh` (ours first: it edits the `.dtb` before genimage makes `sdcard.img`).
- BusyBox fragment `/home/fabio/mz-busybox.fragment` (httpd + CGI, same as `microzed/configs/busybox.fragment`).
- `BR2_ROOTFS_OVERLAY="/home/fabio/mz-overlay"`, root password, `BR2_SYSTEM_DHCP="eth0"`.
- Packages: dropbear, **libgpiod v1** + tools, ethtool, libiio + test programs (project 07).

### Boot chain

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

### Solved setup problems

**libgpiod2 dropped silently.** `BR2_PACKAGE_LIBGPIOD2` was missing from `.config`, and `BR2_TOOLCHAIN_HEADERS_AT_LEAST_5_10` was not set. libgpiod2 needs kernel headers >= 5.10, so menuconfig hides it and silently drops it.
- Fix: use **libgpiod v1** (`BR2_PACKAGE_LIBGPIOD` + "install tools"). It works with older headers. Moving to libgpiod2 later would need newer toolchain headers and a full `make clean && make`.
- v1 tools include `gpiofind`, which v2 does not have. The syntax also differs from v2:

```bash
gpiodetect
gpioset --mode=wait gpiochip0 47=1   # LED D3 on; press Enter to release
gpioget gpiochip0 51                 # button PB1: 0 released, 1 pressed
gpiomon gpiochip0 51                 # watch button events, Ctrl-C to stop
```

(Only while `myled_irq` is not loaded: it owns lines 47 and 51.) `gpiochip0 [zynq_gpio]` has 118 lines: 0-53 MIO, 54-117 EMIO.

**DHCP missing from the image.**
- `BR2_SYSTEM_DHCP="eth0"` was already set in `.config`, but `/etc/network/interfaces` on the board only had `lo`.
- Cause: the file is generated by the `ifupdown-scripts` package, which had been built before the option was set. A plain `make` does not rebuild a package when an option it uses changes.
- Fix: `make ifupdown-scripts-rebuild && make`, then re-flash.
- Manual workaround on a running board: `ip link set eth0 up` then `udhcpc -i eth0`.

## Repo workflow

- Repo `Fuzzyvolt/microzed-linux`, default branch `main`. Each Claude session works on its own branch; merge its pull request into `main` at the end, so the next session sees the work.
- Laptop clone: `~/microzed-linux` (private repo: git asks for the GitHub username and a personal access token). Update with `cd ~/microzed-linux && git pull`.
- In `~/microzed-linux/microzed`: `make` (rebuild our two Buildroot packages), `make push` (copy to the running board), `make overlay`, `make flash` (rebuild the image and write the SD card, with safety checks). Details in `microzed/README.md`.
- `make push` for modules, programs and scripts; `make flash` for device tree, kernel, BusyBox, Buildroot changes, or to make changes permanent.
- `~/mz-overlay` is the rootfs overlay Buildroot uses. `make overlay` copies `microzed/overlay/` and the bitstream into it. It also holds private files that stay out of git: the Dropbear host keys (`etc/dropbear/`, so the key stays the same after re-flashing) and `root/.ssh/authorized_keys`.
- The overlay only adds files. To remove one from the image, delete it from `~/mz-overlay` **and** `~/br-microzed/target/`.

## Board basics

### Network and SSH

- `overlay/etc/network/interfaces`: eth0 DHCP with `pre-up ip link set eth0 address 02:0a:35:00:00:01`, so the router always gives **10.0.0.134**. The MAC is set from Linux because the U-Boot environment lives on the SD boot partition and every `dd` erases it.
- Both boards use this card, so they have the same MAC and IP: never put both on the network at once.
- SSH key: made on the laptop with `ssh-keygen -t ed25519`, copied with `ssh-copy-id root@10.0.0.134` (on the laptop). Kept across re-flashes in `~/mz-overlay/root/.ssh/authorized_keys` (folder 700, file 600; Dropbear ignores looser permissions). `~/.ssh/id_ed25519` is the private key: laptop only.
- Dropbear has no SFTP: use `scp -O`.

### Cross-compiling

- Our programs are built by the `microzed-apps` package (project 06). By hand: `~/br-microzed/host/bin/arm-linux-gcc -Wall -O2 -o blink blink.c -lgpiod` (finds headers and libraries in Buildroot's sysroot). `file blink` should say `ELF 32-bit LSB executable, ARM`.
- Kernel modules: each `microzed/modules/<name>/Makefile` runs the kernel's Kbuild on `~/br-microzed/build/linux-*` with `ARCH=arm CROSS_COMPILE=~/br-microzed/host/bin/arm-linux-`, so `vermagic` matches the running kernel ("invalid module format" otherwise). `make push` copies the `.ko` to `/root/`.

### Web page (BusyBox httpd)

- `http://10.0.0.134` redirects to `/cgi-bin/status`: die temperature, XADC voltage rails, uptime, load, memory, IP, kernel, LED state, PL blocks, buttons (LED on/off/toggle, power off). Refreshes every 5 s. Live ADC chart: `/scope.html`.
- The `control` CGI accepts only POST with exact values (`led_on`, `led_off`, `led_toggle`, `poweroff`) and redirects back with `303 See Other`; it writes `1`/`0`/`t` to `/dev/myled`.
- Not in the default Buildroot BusyBox. Enabled with a config fragment `~/mz-busybox.fragment` (`CONFIG_HTTPD=y`, `CONFIG_FEATURE_HTTPD_CGI=y`), set in menuconfig under Target packages -> BusyBox -> "Additional BusyBox configuration fragment files". Applied with `make busybox-dirclean && make`.
- **BusyBox bug:** with CGI on but `FEATURE_HTTPD_BASIC_AUTH` off, a request for `/` returns 404 even though `index.html` exists (`/index.html` works). httpd appends `index.html`, truncates it for the auth check, and only restores it when basic auth is compiled in. Found by reading the BusyBox source (master branch).
- Workaround used: no `index.html`; `cgi-bin/index.cgi` sends a 302 redirect to `/cgi-bin/status` (httpd only falls back to `index.cgi` when `index.html` does not exist).
- Alternative fix: add `CONFIG_FEATURE_HTTPD_BASIC_AUTH=y` to the fragment, then `make busybox-dirclean && make`.
- `curl` is not on the board; use `wget -O - http://127.0.0.1/...` there.

### XADC (temperature and voltages)

- The kernel's XADC driver shows up as `/sys/bus/iio/devices/iio:device0` (`name` = `xadc`).
- Temperature in degrees C = `(in_temp0_raw + in_temp0_offset) * in_temp0_scale / 1000`.
- Voltages in V = `raw * scale / 1000`. Channels: vccint (1.0 V, PL core), vccaux (1.8 V), vccbram (1.0 V), vccpint (1.0 V, PS core), vccpaux (1.8 V), vccoddr (1.5 V, DDR3), vrefp (1.25 V), vrefn (0 V).

## Projects

### Kernel module basics (before the projects, 2026-11-15)

**hello** (works): `module_init`/`module_exit`, `pr_info` with a `pr_fmt` prefix, a module parameter (`insmod hello.ko who=world`, readable in `/sys/module/hello/parameters/who`), `jiffies`/`HZ` timing. `modinfo hello.ko` shows the license, description, parameter and vermagic. Changing init to `return -ENODEV;` makes the load fail, which is how a driver refuses missing hardware (like macb's "Could not attach PHY (-19)", -19 = -ENODEV).

**myled** character driver: `/dev/myled` controls LED D3. (Removed in the 2026-11-20 cleanup, superseded by myled-irq; last version at commit `6fd32e3`.)
- `echo 1|0|t > /dev/myled` sets on/off/toggle, `cat /dev/myled` prints the state, anything else gives "Invalid argument" (`-EINVAL`).
- Init order: `alloc_chrdev_region` (major:minor, listed in `/proc/devices`) -> `class_create` (`/sys/class/myled`) -> `device_create` (devtmpfs creates `/dev/myled`) -> GPIO lookup table + `gpiod_get` -> `cdev_add` last, so nobody can open the device before the GPIO is ready. The error path (`goto err_...`) and the exit function undo the steps in reverse order.
- User data is copied with `get_user` / `simple_read_from_buffer` (never dereference a user pointer). A mutex protects the LED state.
- The LED pin was hard-coded with a `gpiod_lookup_table` (chip label `zynq_gpio`, line 47). The next step replaces this with a device tree node.
- Loading it while another user holds the GPIO fails with "Device or resource busy" (`-16`, -EBUSY): the kernel allows one owner per GPIO line.
- `class_create()` lost its first argument (`THIS_MODULE`) in kernel 6.4, handled with a `LINUX_VERSION_CODE` check.

**myled-dt** device tree + platform driver: same `/dev/myled`, but the hardware comes from a device tree node. (Removed in the cleanup like myled; myled-irq does the same and more.)
- `microzed/board/post-image-dt.sh` adds this node to the built `.dtb` with `fdtput` (from `device-tree-compiler`), giving the GPIO controller a `phandle` if it has none:
  ```
  myled { compatible = "microzed,myled"; label = "d3"; led-gpios = <&gpio0 47 0>; };
  ```
  Editing the compiled `.dtb` avoids depending on the kernel's `.dts` file location and keeps the `.dtb` name the boot setup expects.
- Buildroot setting: see "Buildroot setup" (**order matters**: our script before `board/zynq/post-image.sh`, which runs genimage).
- Check before flashing: `fdtget ~/br-microzed/images/*.dtb /myled compatible`. On the board: `ls /proc/device-tree/myled/`; the platform device exists even before the driver loads (`/sys/bus/platform/devices/`).
- The driver only registers itself in init; the kernel calls `probe()` for each node whose `compatible` matches `of_match_table`. `probe()` reads `label` (`device_property_read_string`) and `led-gpios` (`devm_gpiod_get(dev, "led", ...)`), then creates `/dev/myled`. `devm_*` resources are freed automatically on unbind. Binding shows in `/sys/bus/platform/drivers/myled/`.
- `platform_driver.remove` returns void from kernel 6.11 (int before), handled with `LINUX_VERSION_CODE`.

**myled-irq** button interrupt driver (in use): adds PB1 to the same node (`button-gpios = <&gpio0 51 0>`, written by `post-image-dt.sh`, which now updates an existing node instead of skipping it).
- `/dev/mybutton`: `read()` sleeps until PB1 is pressed, then returns `press N`. Supports `poll()`/`select()` and `O_NONBLOCK` (`-EAGAIN`). Each `open()` gets its own "last press seen" counter, so several readers each get every press. `/dev/myled` works as before. Both use one major number (minor 0 = myled, minor 1 = mybutton).
- Interrupt: `gpiod_to_irq()` gives the button's IRQ; `devm_request_threaded_irq(..., NULL, thread_fn, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT, ...)`. The threaded handler (bottom half) may sleep: it waits 20 ms (`msleep`), reads the settled value, and on a 0 -> 1 change toggles the LED, increments an atomic press counter and calls `wake_up_interruptible()`. A plain top-half handler runs in interrupt context and may not sleep. `IRQF_ONESHOT` keeps the line masked until the thread returns.
- Readers sleep in `wait_event_interruptible()` on a wait queue, using no CPU; Ctrl-C (a signal) ends the wait with `-ERESTARTSYS`.
- `/proc/interrupts` shows the `myled-button` line: about two interrupts per press (press + release, plus bounce), while the debounced press count rises by one.
- Test: `cat /dev/mybutton`, or `while read p < /dev/mybutton; do echo "Got $p"; done`.
- This is the pattern a DMA or ADC driver uses: the hardware interrupts when data is ready, the handler wakes the reader, `read()` returns the data.

**Driver loaded at boot:**
- `/etc/init.d/S30myled` loads it with `modprobe myled_irq` (start/stop/restart by hand). Until project 06 it used `insmod` with a path: Buildroot runs `depmod` before copying the overlay, so a module in the overlay is never indexed.
- The web page writes `1`/`0`/`t` to `/dev/myled` and reads the LED state from it. Full chain: browser -> httpd -> CGI -> `/dev/myled` -> our driver -> GPIO -> LED.

**LED daemon** (`leddaemon`, retired on 2026-11-15 for myled-irq, removed in the cleanup; last at commit `6fd32e3`): a user-space daemon started by `S99leddaemon` that owned the LED and button with libgpiod, debounced PB1 (30 ms quiet), took `on|off|toggle` through a FIFO and wrote its state to a file for the CGI. Lesson kept: a daemon is started once and owns the hardware; a CGI runs once per request. Real devices combine them: the daemon (or a driver) owns the hardware, the CGI talks to it.

### Project 01: Vivado (done 2026-11-15)

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

### Project 02: AXI register block (done 2026-11-17)

```
CPU (PS) --M_AXI_GP0 (AXI3)--> AXI Interconnect --(AXI4-Lite)--> axiregs (PL)
                               (protocol converter)              0x43C00000, 4 KB
FCLK_CLK0 (100 MHz) clocks the PL side; proc_sys_reset makes the reset.
```

Register map: `microzed/fpga/README.md` (reg0, reg1, sum, count, id `0x4D5A0201`).

Files (all in `microzed/`):
- `fpga/02-axiregs/axiregs.v`: hand-written AXI4-Lite slave. Added to the block design as a **module reference** (`create_bd_cell -type module -reference axiregs`), not with "Create and Package New IP": one readable file in git, no generated boilerplate. The `s_axi_*` port names make Vivado see them as one AXI interface; `X_INTERFACE_*` attributes tie the clock and reset to it.
- `fpga/02-axiregs/tb_axiregs.v`: testbench (the AXI master role). `iverilog -o /tmp/tb tb_axiregs.v axiregs.v && vvp /tmp/tb` prints PASS lines.
- `fpga/02-axiregs/build.tcl`: PS with board preset + `PCW_USE_M_AXI_GP0 1`, axiregs, connection automation (AXI Interconnect + Processor System Reset), address `0x43C00000`/4K, bitstream, `axiregs.bin`, `.xsa`. Works with Vivado 2026.1: `cd ~/microzed-linux/microzed/fpga/02-axiregs && vivado -mode batch -source build.tcl`. Output in `~/vivado/mz02_axiregs/`.
- `fpga/02-axiregs/bit2bin.tcl`: `.bit` -> `.bin` for the FPGA manager (done: projects 01-03, own registers instead of AXI GPIO; PWM needs a breakout). A `.bit` is a text header (design, part, date) + config data; the PCAP wants only the data with each 32-bit word byte-swapped, so the sync word `AA 99 55 66` reads `66 55 99 AA`. Replaces `bootgen -process_bitstream bin`, which needs Vitis (not installed).
- `modules/axiregs/axiregs.c`: platform driver, `compatible = "microzed,axiregs"`.
- `board/post-image-dt.sh`: now also adds `axiregs@43c00000 { compatible; reg = <0x43c00000 0x1000>; clocks = <&clkc 15>; fpga-mgr = <&devcfg>; }`. The script has a `phandle()` helper (clkc and devcfg already have phandles, the GPIO controller gets one).
- `overlay/etc/init.d/S40axiregs` (replaced by `S40pl` in project 03): programs the PL, then loads the driver.
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

### Project 03: PL timer with interrupt (done 2026-11-17)

```
                                         +--> axiregs_0  0x43C00000 (project 02, kept)
CPU --M_AXI_GP0--> AXI Interconnect -----+
                                         +--> pltimer_0  0x43C10000
GIC <-- IRQ_F2P[0] = GIC ID 61 <-------------------- irq (level high)
```

Register map: `microzed/fpga/README.md` (CTRL, PERIOD, STATUS W1C, TICKS, COUNT, ID).

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

### Project 04: AXI DMA loopback (done 2026-11-18)

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

### Project 05: DMA data stream, simulated ADC (done 2026-11-19)

```
 PL                                                          PS
 adcsim_0 (0x43C20000) --stream--> axis_fifo --> axi_dma_0 S2MM --HP0--> ring of 8 DDR buffers
  1 sample every DIV clocks         8192 x 32    (xilinx_dma)            |
  TLAST every PKT_LEN samples                                            +--> /dev/adcstream --> adcread
  fifo_aresetn --> FIFO reset (FLUSH)                                    +--> sysfs snapshot --> /scope.html
```

The project 04 design with the FIFO's input moved: `adcsim` feeds S2MM; the DMA's MM2S side is kept (unused) so the DMA still matches its device tree node. axiregs ID `0x4D5A0501`. Built first time with Vivado 2026.1.

adcsim registers: `microzed/fpga/README.md`.
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

### Project 06: Buildroot packages (done 2026-11-19)

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
- Checked in the cloud session against a clone of Buildroot 2026.02.x (config parses, dependencies and hooks as expected), then on the laptop and board: `make clean && make flash` built both packages, the modules are in `target/lib/modules/<version>/microzed/`, they load at boot, and `rmmod myled_irq && modprobe myled_irq` works (LED toggles).
- Day-to-day: edit `modules/` or `apps/`, then `make push` (quick test) or `make flash` (permanent). After a kernel change, run `make` in `microzed/` too (Buildroot does not rebuild packages that depend on the kernel by itself).
- Not done (optional): automatic module loading from the device tree (`MODULE_DEVICE_TABLE` aliases + mdev/eudev). Fine for `myled_irq`; the PL drivers must still wait for the bitstream.
- The per-module Makefiles in `modules/*/` still work for quick experiments (`insmod /root/x.ko`); `make clean` in `microzed/` removes their leftovers.

### Project 07: the ADC as an IIO device (done 2026-11-19)

```
adcsim (PL) --> FIFO --> AXI DMA S2MM --> IIO DMA engine buffer --> /dev/iio:deviceN --> iio_readdev, adcread -i
0x43C20000                (xilinx_dma)     (kernel helper, 2 blocks)   + /sys/bus/iio/devices/iio:deviceN/
```

Status: done 2026-11-19 on board 2. Compiled in the cloud session against linux-xlnx `xlnx_rebase_v6.12_LTS` (6.12.70) with `xilinx_zynq_defconfig` + `configs/linux.fragment` (ARM cross-compiler, `W=1`, no warnings; full kernel build, `depends=industrialio-buffer-dmaengine`), then tested on the board. One bug found and fixed on the board (stop order, below).

Results (board 2, second version of the driver):
- `iio_info`: `iio:device1: adcsim (buffer capable)`, `voltage0: (input, index: 0, format: le:s16/32>>0)`, `scale 0.030517578`, `signal_frequency 1000`, `signal_mode sine`, `signal_mode_available sine saw`, plus `block_samples`, `pl_dropped`, `pl_packets`, `pl_samples`, `sampling_frequency`.
- `iio_readdev -b 8192 -s 1000000 adcsim > /tmp/adc.bin`: `adcread -f` 999,424 samples (122 whole blocks), `lost=0`. On the laptop `tools/iio_check.py --plot`: lost 0, 1000 Hz, clean sine.
- `adcread -i -t 30`: 1 MSa/s, then 5 MSa/s set through `sampling_frequency` while streaming: 0 lost, 0 PL drops.
- `adciio-bench 10 16384` (64 KiB blocks, every sample checked):

| Rate | Measured Sa/s | MB/s | Lost | PL dropped | CPU % |
|---|---|---|---|---|---|
| 100 k | 99,995 | 0.4 | 0 | 0 | 0 |
| 1 M | 999,958 | 4.0 | 0 | 0 | 1 |
| 5 M | 4,999,751 | 20.0 | 0 | 0 | 9 |
| 10 M | 9,999,510 | 40.0 | 0 | 0 | 19 |
| 14.3 M | 14,285,116 | 57.1 | 0 | 0 | 28 |
| 20 M | 19,998,937 | 80.0 | 0 | 0 | 39 |
| 25 M | 24,998,902 | 100.0 | 0 | 0 | 49 |
| 33 M | 27,387,046 | 109.5 | 59.4 M | 59.4 M | 50 |
| 50 M | 27,554,181 | 110.2 | 224 M | 224 M | 50 |
| 100 M | 27,517,095 | 110.1 | 725 M | 725 M | 50 |

- **Same limit as project 05's best `read()` path** (no loss up to 25 MSa/s, ceiling ~27.5 MSa/s = 110 MB/s vs 26.6), with the stock kernel buffer and only two DMA blocks. The limit is again one core at 100% (adcread). 64 KiB blocks + the 8192-word PL FIFO are enough slack.
- `lost` (sequence numbers) and `pl_dropped` (PL counter) agree: with the IIO buffer every loss is a PL drop.

What IIO is: the kernel's framework for ADCs, DACs and sensors. The driver describes its channels (type, sign, bits, scale); the IIO core makes the sysfs files, the `/dev/iio:deviceN` node and the buffer. Generic tools (libiio: `iio_info`, `iio_attr`, `iio_readdev`) then work with any IIO ADC. The XADC is already one (`iio:device0`, name `xadc`).

Files (all in `microzed/`):
- `modules/adciio/adciio.c`: platform driver, same compatible `microzed,adcstream` as adcstream (a compatible names the hardware, not the driver; whichever module is loaded binds). IIO device name `adcsim`, one channel `in_voltage0`: `le:s16/32>>0` (16-bit signed value in a 32-bit word; the top 16 bits still carry the sequence number). Attributes: `sampling_frequency` (+ `_available`, writable while streaming), `in_voltage0_scale` (1000/2^15 mV per LSB, a simulated ±1 V range), our own `in_voltage0_signal_frequency`, `in_voltage0_signal_mode` (`sine`/`saw`, an `IIO_ENUM`), `pl_samples`, `pl_dropped`, `pl_packets`, `block_samples`.
- `configs/linux.fragment`: `CONFIG_IIO_BUFFER_DMA=m`, `CONFIG_IIO_BUFFER_DMAENGINE=m` (checked: `olddefconfig` keeps both as `=m`; `CONFIG_IIO=y` and `CONFIG_IIO_BUFFER=y` already come from the XADC driver).
- `overlay/etc/init.d/S40pl` + `overlay/etc/default/pl`: `ADC_DRIVER=adcstream` (default, web chart) or `adciio`. One-off: `/etc/init.d/S40pl stop; ADC_DRIVER=adciio /etc/init.d/S40pl start`.
- `apps/adcread.c`: `-i` reads the IIO device itself (sets `scan_elements/in_voltage0_en`, `buffer/length` = 2 x block, `buffer/enable`), `-b` block size; `-f file` / `-f -` checks a recording or a pipe from `iio_readdev`.
- `overlay/usr/bin/adciio-bench`: like `adcstream-bench`, through `adcread -i`.
- `tools/iio_check.py` (laptop, numpy): lost samples, min/max, FFT frequency, sine fit, `--plot`.
- Status web page shows the IIO device when adciio is loaded. `/scope.html` needs adcstream.

How the buffer fits the hardware (read from the 6.12 source, `drivers/iio/buffer/industrialio-buffer-dma*.c`):
- `iio_dmaengine_buffer_setup(dev, indio_dev, "rx")` requests the S2MM channel (from `dma-names`) and does all the DMA work: `dmaengine_prep_slave_single()` + submit + `issue_pending` per block, completion callback, hands done blocks to `read()`.
- With `read()` (all libiio 0.x does) it uses exactly **two blocks** of `buffer/length / 2` samples, capped at 64 KiB (the DMA device's default max segment size): one filling, one being read. That matches xilinx_dma's simple-mode limit (one queued behind the running one).
- Slack is one block. A late reader leaves the DMA with no block, the PL FIFO overflows: loss shows as `pl_dropped`, not as overruns.
- Simple mode reports no residue, so the PL packet (TLAST every `PKT_LEN`) must be exactly one block. `postenable` reads the size the DMA was actually given (`blocks[0]->bytes_used`, from the queue struct in `buffer-dma.h`) and writes it to `PKT_LEN` before starting the PL. `preenable` refuses an odd `buffer/length` (half-sample blocks).
- Stop: the core calls `predisable`, then `dmaengine_terminate_sync()` (halt + reset of the channel), then `postdisable`. `predisable` (second version): (1) `queue->active = false` under `queue->lock`, so blocks handed back by `read()` no longer go to the DMA; (2) DIV = 1 until no read() block is ACTIVE (< 1 ms); (3) stop the PL and empty its FIFO. The DMA is then idle (no hang in terminate) and nothing flows into it after its reset. Side effect: the last block(s) of a recording hold 100 MSa/s samples. A failure must never happen after the blocks are queued (`postenable` cannot fail): the core would then stop a DMA that waits for data.
- Bug found on the board (first version): `predisable` only set DIV = 1 and the PL kept running until `postdisable`, i.e. through the DMA reset. Result: the first stream after a module load was clean, the second hung; dmesg `xilinx-vdma 40400000.dma: Channel ... has errors 10` = DMASR bit 4 DMAIntErr. Explanation (not proven in detail): words taken in by the DMA after its reset sit in front of the next stream's first packet, the packet no longer fits the block, the DMA flags an internal error, xilinx_dma sets `chan->err` and starts nothing until the next terminate, so `read()` waits forever. Fixed by stopping the PL before the core's terminate (same order as adcstream's stop).
- Core quirk: the IIO DMA buffer never resets `fileio.next_dequeue`. After a stream that ended on an odd block (e.g. `iio_readdev -s 1000000`), the next stream reads block 1 before block 0 (the DMA fills 0 first): swapped first blocks, a burst of "lost" at the start (seen once: `lost 131068` in the first second, nothing after). `preenable` now sets it to 0.
- A hung reader keeps `/dev/iio:deviceN` open: the next open gives "Device or resource busy" and `rmmod adciio` fails. Kill it (`killall adcread`) first.
- libiio 0.26 (Buildroot 2026.02) writes `buffer/length` = 4 x the `-b` size, so `iio_readdev -b 8192` gives 64 KiB blocks. It prints "High-speed mode not enabled": expected (that is ADI's out-of-tree mmap interface). `buffer/watermark` is read-only here; libiio accepts that.

Buildroot setup for this project (laptop, once): `make menuconfig` in `~/br-microzed` -> Target packages -> Libraries -> Hardware handling -> `libiio`, with "Install test programs" on (`iio_info`, `iio_attr`, `iio_readdev`, ...); the USB and serial backends can be turned off. "IIO Daemon" (iiod) is optional: it would let the laptop use libiio over the network (`iio_info -u ip:10.0.0.134`). Then `make linux-reconfigure` (kernel fragment) and `make flash` in `microzed/`.

### Project 08: network stream to the laptop (done 2026-11-20)

```
board                                                   laptop (Wi-Fi)
/dev/adcstream --> adcserve --TCP port 5000-->  tools/adcrecv.py  (check, record, --plot)
                           \--UDP port 5000-->  tools/adcrecv.py --udp
                   S95adcserve starts both at boot  tools/adcnet-bench (rates over ssh)
```

Files (all in `microzed/`):
- `apps/adcserve.c` (in the `microzed-apps` package): opens `/dev/adcstream` only while a laptop receives; settings stay in sysfs. Options: `-u` UDP, `-p` port, `-m` zero copy (mmap + ioctl, sends straight from the DMA buffer), `-S` SO_SNDBUF, `-d` samples per datagram, `-q`. Prints per second (or per session with `-q`): Sa/s, MB/s, overruns, PL drops, board CPU (from `/proc/stat`, 50% = one core), datagrams not sent.
- Protocol: one text line `ADCSERVE 1 proto=tcp rate=... block=... freq=... mode=... dgram=... format=u32le`, then raw 32-bit words (same format as `iio_readdev`). UDP: the laptop sends `start`, gets the line as a datagram, then datagrams of `MZD1` + datagram number (u32) + sample count (u32) + samples, 365 samples = 1472 bytes = one Ethernet frame; one `sendmmsg()` for up to 64 datagrams, each datagram two iovecs (header + slice of the block, no extra copy). The laptop sends `keep` every second; the board stops 3 s after the last one, or at `stop`.
- `tools/adcrecv.py` (laptop, numpy): sequence check (vectorised), per-second rate/losses, `-o` raw recording, `--plot` live time plot (last 2000 samples) + FFT (last 65536) in matplotlib, receiving in a second thread. UDP: datagrams received in batches into slots of one big buffer, checked with numpy; batch sorted by datagram number (Wi-Fi reorders).
- `tools/adcnet-bench [--udp] [seconds] [block_samples]` (laptop): sets each rate over ssh, runs adcrecv, reads the board's CPU, overruns and PL drops before/after.
- `overlay/etc/init.d/S95adcserve` + `overlay/etc/default/adcserve` (`ADCSERVE=no` to turn off, `ADCSERVE_OPTS`, `ADCSERVE_SNDBUF` default 32 MiB, also written to `net.core.wmem_max`). Logs `/var/log/adcserve-{tcp,udp}.log`. `make push` stops the servers before copying (else "Text file busy") and starts them after.
- `tools/iio_check.py`: sine fit now over the first 0.1 s only (over 10 s a frequency error far below the FFT bin drifts the phase and the fit failed: "not a clean sine" on a perfect one).
- Tested in the cloud session with a fake device (FIFO + fake sysfs, adcserve built for x86): TCP and UDP byte-identical, injected gaps counted exactly; ARM cross-compile clean.

Results (board 2, laptop on Wi-Fi, 2026-11-20):
- TCP 1 MSa/s, 300 s: laptop `samples=299995136 lost=0`, board `overruns=0 pl_dropped=0`, CPU 4%.
- UDP 1 MSa/s, 60 s, 4096-sample blocks: `lost=0 dgram_lost=0`; 176,976 datagrams sent, 176,928 received: the 48 missing = 4 blocks in flight after `stop`. CPU 7% (TCP 4%: many small packets vs TCP's large segments).
- Recording 10 s: `iio_check.py` lost 0, 1000.0 Hz, clean ±1000 mV sine.

`adcnet-bench`, 10 s per rate:

| Rate | TCP, 32768 blocks | UDP, 4096 blocks | UDP, 32768 blocks |
|---|---|---|---|
| 100 k | 0 lost | 0 lost | 245 datagrams lost |
| 1 M .. 5 M | 0 lost | 0 lost | 4 .. 76 datagrams lost |
| 10 M | 0 lost, 40 MB/s, CPU 27% | ~6.1 MSa/s only, overruns | 2088 datagrams lost |
| 14.3 M | at the edge (0 or 2 overruns; 73k PL drops once) | overruns | overruns |
| ceiling | ~15 MSa/s = 60 MB/s, CPU ~40% | ~6 MSa/s = 24 MB/s, CPU 48% | ~10.8 MSa/s = 43 MB/s, CPU 65% |

- **Highest lossless rate: TCP 10 MSa/s (40 MB/s), UDP 5 MSa/s (4096 blocks).** With 32768-sample blocks UDP is never lossless on Wi-Fi.
- **TCP's limit here is the Wi-Fi** (~60 MB/s ≈ 480 Mbit/s): `-m` (zero copy) gave the same ceiling, board CPU stayed under one core (~40%) and fell at the top rates (waiting for the network).
- **UDP bursts:** one block goes out as one burst at gigabit speed (32768 samples = 90 datagrams = 132 KB); the Wi-Fi router cannot queue it and drops some, even at 100 kSa/s. 4096-sample blocks (bursts of 12) lose nothing, but cost one core at ~6 MSa/s (a read, a sendmmsg and a command check per block). TCP paces itself to the network, UDP does not.

Problems found on the way (fixed):
- **Tiny TCP send buffer.** First 30 s run: laptop `lost=12288` (3 blocks), board `overruns=19`. The log said `socket send buffer 44800 bytes` = 11 ms at 4 MB/s; a Wi-Fi pause blocked `send()`, the driver's ring (~25 ms with 4096 blocks) overflowed. Fix: 32 MiB SO_SNDBUF (Linux caps it at `net.core.wmem_max`, ~200 KB by default, so S95adcserve raises that; getsockopt reports double: 67108864). About 4 s of slack at 1 MSa/s.
- **19 overruns but only 3 blocks lost on the laptop:** the 16-bit sequence number wraps every 65536 samples = 16 blocks of 4096; a burst of 17 lost blocks shows as 1. Trust zero only when both laptop `lost=0` and board `overruns=0`.
- **Wi-Fi reorders UDP datagrams:** a swap looked like a gap of ~65535 samples (sequence going backwards) and one lost datagram. adcrecv now sorts each batch by datagram number.
- **Stale pid file:** `start-stop-daemon -K -p` said "killing process 323: No such process" while adcserve ran. `S95adcserve stop` now uses `killall adcserve`.
- `make push` asked for the password many times: the key was no longer on the board; `ssh-copy-id root@10.0.0.134` fixed it (the overlay copy in `~/mz-overlay/root/.ssh/` keeps it across re-flashes).

Open:
- **PL drops under heavy network load:** at 14.3 MSa/s TCP the board showed `overruns=0` but `pl_dropped=73231` once: the DMA had no free buffer for > 0.6 ms (FIFO depth at that rate). Guess (not proven): network interrupt/softirq work delays the DMA completion callback on the same core. Test: put the DMA and Ethernet interrupts on different cores (`/proc/irq/N/smp_affinity`).
- Board's own network limit not measured (laptop has no Ethernet port): needs a USB 3 gigabit adapter. Expect 100+ MB/s with `-m`.
- Ethernet pause frames during heavy streaming not checked yet (`ethtool -S eth0 | grep -i pause` before/after).

## Hardware problems

### Ethernet PHY not detected at power-up (board 1, open)

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

### Wi-Fi disruption while the board is on the network (open, watching)

- Symptom: while the board is connected to the Vidéotron router, a video call on another computer (on Wi-Fi) stops. Not an IP conflict (the other computer has a different IP), and only one `Link is Up` in `dmesg` (no link flapping).
- Suspect: Ethernet **pause frames**. `ethtool eth0` shows the board advertises `Transmit-only` pause and the router `Symmetric Receive-only`, so the board may send pause frames and the router obeys them (boot log: `flow control tx`). On home routers this can stall the shared switch, including Wi-Fi traffic.
- `ethtool -a eth0` gives "Operation not supported" (the macb driver has no pause settings), so pause is turned off through the PHY advertisement instead: `ethtool -s eth0 advertise 0x02f` (10/100 half+full and 1000 full, no Pause/Asym_Pause bits). Then `dmesg | tail` should show `flow control off`.
- Not applied yet: `ethtool -S eth0 | grep -i pause` showed no pause frames so far. Next time the problem happens, check whether `tx_pause_frames` climbs; if so, apply the advertise fix and make it permanent with a `post-up` line in `overlay/etc/network/interfaces`.
- Other test: stop the board at the U-Boot prompt during a call. If the call still drops, the PHY itself causes it (hardware).

### SD card corruption

- The same SD card boots on a second MicroZed (same PS for 7010/7020; set the boot jumpers to SD first).
- On the first try it panicked: `EXT4-fs (mmcblk0p2): group descriptors corrupted!` / `VFS: Unable to mount root fs on unknown-block(179,2)` (179,2 = mmcblk0p2, the rootfs).
- Cause: power was cut while the board was writing to the card (easy to do with all the PHY power-cycling). Fixed by re-flashing (or `sudo fsck.ext4 -fy /dev/sdb2`).
- **Always run `poweroff` and wait for `reboot: System halted` before unplugging.**

## Next steps

Projects 01 to 08 are done (briefs in `microzed/projects/`). Next session: write the brief first (`microzed/projects/09-*.md`). Choices:
- A follow-up from the lists below.
- A hardware project once a MicroHeader breakout is available: SPI ADC or pressure sensor sampled by the PL in place of adcsim (same drivers), PWM outputs.
- QSPI boot, read-only rootfs, watchdog, FreeRTOS/AMP (see "Longer roadmap").

Start a session with: "Read `buildroot-microzed-project.md` and `microzed/projects/<brief>.md`, then let's do it." End it with "update the md".

Still open on the side: watch the pause counters during the next Wi-Fi disruption; PHY test on board 2 (5 cold boots with the cable plugged in).

Optional follow-ups, by project:
- 08 network:
  - Wired test with a USB 3 gigabit Ethernet adapter on the laptop: the board's own TCP/UDP limit, `-m` vs `read()`.
  - PL drops under network load: DMA and Ethernet interrupts on different cores (`grep -E 'dma|eth' /proc/interrupts`, `echo 2 > /proc/irq/N/smp_affinity`).
  - UDP pacing (spread datagrams over the block time, or `SO_MAX_PACING_RATE` with the `fq` qdisc), or a second thread so reading and sending use both cores.
  - Live plot extras: trigger, time window, rate/frequency controls.
  - iiod (libiio's network daemon) + `iio_readdev -u ip:10.0.0.134 adcsim` on the laptop, compared with adcserve.
  - A 64-bit sample counter in the stream (per-block header), so whole lost blocks never hide behind the 16-bit wrap.
- 07 IIO:
  - Scan type matching a real ADC part; a timestamp per block.
  - More than two blocks: only with the DMABUF interface (libiio 1.x) or scatter-gather.
  - Upstream report: the IIO DMA buffer never resets `fileio.next_dequeue` (we reset it in `preenable`).
- 05 stream:
  - Cached buffers + `dma_sync_single_for_cpu()` (fast per-sample reads with zero copy); a second thread; NEON for min/max/sum.
  - Scatter-gather mode (`c_include_sg 1`, M_AXI_SG on HP0): many buffers queued in hardware. Cyclic mode in xilinx_dma needs SG.
  - A bigger `nbuf` vs. overruns at 15..17 MSa/s; `adcread` counting overruns into `lost`.
- 04 DMA: re-run the 4 MiB size; 64-bit stream/memory width (ceiling 800 MB/s); `dma_map_single()` + cached buffers vs coherent.
- 03 timer: stress test at `period_us` 50; user-space jitter vs the driver's ±3.5 µs.
- 06 packages: automatic module loading from the device tree (`MODULE_DEVICE_TABLE` + mdev/eudev); the PL drivers must still wait for the bitstream.

## Longer roadmap

1. Linux on hardware: GPIO, SSH, U-Boot environment (done), QSPI boot (not yet).
2. Kernel: "hello" module, character driver, device tree edits (done).
3. FPGA: Vivado (free edition supports the 7010; tens of GB download), AXI GPIO/PWM in the PL, controlled from Linux (UIO, then a kernel driver), FPGA manager (done: projects 01-03, own registers instead of AXI GPIO; PWM needs a breakout).
4. AXI DMA: stream data from the PL into DDR and read it from Linux (done: projects 04-08).
5. Bare-metal Cortex-A9 and FreeRTOS, then Linux + FreeRTOS on separate cores (OpenAMP).
6. Small instrument: SPI ADC or pressure sensor, DMA, Linux, streaming over Ethernet (all done with the simulated ADC; a real sensor needs a breakout); optionally Modbus RTU over RS-485.

Note: most PL (FPGA) pins only reach the MicroHeaders on the underside, so PL I/O needs a carrier card or breakout. The LED, button and Pmod J5 are on the PS side.

## Lessons learned

### Buildroot

- Run `make menuconfig` from the folder of the build you mean to change (`~/br-microzed` for the board). Afterwards, check with `grep` on that folder's `.config`.
- After `make olddefconfig` or menuconfig, check that options you added are still `=y`. Unmet dependencies drop them silently.
- Rebuild rules: new package means `make`; one changed package means `make <pkg>-rebuild && make`; toolchain or arch change means `make clean && make`.
- A plain `make` does not rebuild a package when a menuconfig option it uses changes (example: `BR2_SYSTEM_DHCP` and `ifupdown-scripts`). Rebuild that package. If unsure which package uses the option, `make clean && make`.
- Before re-flashing, check the result in `~/br-microzed/target/` (for example `ls target/usr/bin | grep gpio`, `cat target/etc/network/interfaces`).
- A Buildroot package can be hidden in menuconfig because of a toolchain dependency (for example libgpiod2 needs headers >= 5.10). Search with `/` to see its "Depends on" line.
- The overlay is copied during `make`. A file added to `~/mz-overlay` after the last `make` is not in `sdcard.img` until you run `make` again.
- BusyBox options are changed with a config fragment, then `make busybox-dirclean && make` (a plain `make` does not reconfigure an already-built BusyBox).
- The overlay only adds files. To remove something from the image, delete it from `~/mz-overlay` **and** from `~/br-microzed/target/`.
- A new device tree node needs `make flash` (post-image script); a new module or script alone only needs `make push`.
- Kernel options: Buildroot "Additional configuration fragment files" + `make linux-reconfigure && make`; check with `grep` on `~/br-microzed/build/linux-*/.config`. After a kernel rebuild, run `make` in `microzed/` (Buildroot does not rebuild kernel-dependent packages by itself).
- Ship drivers as Buildroot packages (`BR2_EXTERNAL`, `$(eval $(kernel-module))` + `$(eval $(generic-package))`): they are built against the exact kernel and indexed by `depmod`, so `modprobe` works. Files added through the overlay are never indexed (depmod runs before the overlay).
- `*_SITE_METHOD = local`: Buildroot copies the source once and never notices edits; `make <pkg>-rebuild` copies it again. Exclude build leftovers with `<PKG>_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS`.
- An old file in the overlay overrides the package's version (the overlay is copied last): remove retired programs and modules from `~/mz-overlay` when they move into a package.

### Board, shell and network

- Before `dd`, check the SD card name with `lsblk` and unmount its partitions. Write to the whole disk (`/dev/sdb`), not a partition.
- Boot jumpers are read only at power-on.
- `picocom` "Permission denied" means the `dialout` group isn't active yet (log out/in, or `newgrp dialout`). "No such file" means the board isn't connected or the port has another name (check `lsusb`, `ls /dev/ttyUSB*`, `sudo dmesg | tail`).
- Quit picocom: `Ctrl-A` then `Ctrl-X`.
- Save and exit menuconfig: `Esc Esc` until it asks to save, then Yes.
- Re-flashing creates a new Dropbear host key. Clear the old one on the PC with `ssh-keygen -R <ip>`.
- Find the board on the network without serial: `nmap -p 22 --open 10.0.0.0/24`.
- Power up the MicroZed without the Ethernet cable, then plug it in (see the PHY issue above).
- Long `cat << EOF` pastes through KDE Connect can get cut off. For long files, paste into `nano` (Ctrl+Shift+V, save Ctrl+O, exit Ctrl+X) and check with `sh -n` and `tail`.
- Run `poweroff` before unplugging, or the ext4 rootfs can get corrupted.
- End an SSH session with `exit` or Ctrl-D; force-close a frozen one with Enter, `~`, `.`.
- Board shell errors look like `-sh: <cmd>: not found`. If a laptop command (`ssh-copy-id`, `curl`, `make`) gives that, you are in the board's SSH session: `exit` first. Prompt `#` = board, `fabio@fabio-UX430UAR:~$` = laptop.
- A background command in a CGI script must redirect its input and output (`</dev/null >/dev/null 2>&1 &`), or httpd waits for it and the page hangs.
- Copying over a running program fails with "Text file busy": stop it first.
- `poweroff` stops services, syncs and halts Linux, but the MicroZed has no software power switch: the LEDs stay on. Wait for `reboot: System halted` (serial) or for SSH/ping to stop, then unplug.
- Recent OpenSSH `scp` uses SFTP, which Dropbear does not have: use `scp -O`.
- BusyBox `start-stop-daemon -m -p` pid files can go stale: stop daemons with `killall <name>` in init scripts.

### Vivado and the PL

- Vivado 2026.1 on Mint 22: needs `libtinfo5` **and** `libncurses5` from Ubuntu 22.04 (`libncurses.so.5: cannot open shared object file` otherwise). Package file names change with security updates (`ubuntu0.1` -> `ubuntu0.3`): look in the pool folder if a URL gives 404.
- AMD's `installLibs.sh` ships with Windows line endings (`sudo: unable to execute ...: No such file or directory`, `head -1 | cat -A` shows `^M$`). Run a cleaned copy: `tr -d '\r' < installLibs.sh > /tmp/installLibs.sh`, then `sudo bash /tmp/installLibs.sh`.
- Vivado Tcl does not expand `~` in `set_param board.repoPaths [list ...]`; use the full path. Test with `get_board_parts *microzed*` in the Tcl console.
- `vivado: command not found` in a new terminal: run `vivado-env` (or source `settings64.sh`) first.
- **Never read a PL address (`devmem`, driver) before the bitstream is loaded**: nothing answers on the AXI bus and the CPU hangs until a power cycle. Check `cat /sys/class/fpga_manager/fpga0/state` = `operating` first. The driver checks this itself.
- The Zynq FPGA manager wants a byte-swapped `.bin`, not the `.bit` ("Invalid bitstream, could not find a sync word" otherwise). `bit2bin.tcl` makes it without Vitis.
- Write custom PL blocks as plain Verilog + module reference in the block design; simulate them first (Icarus `iverilog`, or Vivado's simulator). Much faster than a bitstream build to find AXI handshake bugs.
- When a new bitstream replaces the old one, keep the old blocks in the new design (only one bitstream at a time) and keep their addresses, so the old drivers still work.
- AXI DMA "buffer length register width" defaults to 14 bits (16 KB per transfer). Set it in Vivado and in DT `xlnx,sg-length-width` to the same value.
- The DMA's address window to DDR must cover all RAM (1 GB): CMA buffers sit near the top (~0x3F000000).
- Vivado Tcl wiring by hand (`connect_bd_intf_net`, `connect_bd_net`, `axi_interconnect` with `NUM_SI`/`NUM_MI`) is predictable and readable; connection automation is fine for a single slave but picks names and interconnect types itself.

### Kernel drivers and DMA

- A level interrupt from the PL must be cleared in the device (W1C) by the handler, followed by a read-back, or it fires forever.
- Device tree SPI number = GIC ID - 32. Zynq IRQ_F2P[0] = ID 61 = `<0 29 4>`.
- A driver that reads PL registers in `probe()` must not be built into the kernel when its DT node is in the boot `.dtb` and the PL is loaded from Linux: it binds before the bitstream and hangs the CPU. Make it a module (kernel config fragment) and load it after the bitstream. Xilinx's AXI DMA driver is one (`CONFIG_XILINX_DMA=y` in `xilinx_zynq_defconfig`).
- DMA buffers: allocate with the DMA controller's device (`chan->device->dev`), use coherent memory or map + sync (the HP ports bypass the caches); start the receive channel before the transmit one.
- xilinx_dma in simple (non-SG) mode: queue at most one descriptor behind the running one, and call `dma_async_issue_pending()` after each submit. Several queued descriptors are all "completed" by one interrupt.
- Before `dmaengine_terminate_sync()` on an S2MM channel, make sure it holds no buffer (feed it data until done). A channel waiting for stream data never halts, and xilinx_dma's halt timeout never expires (`readl_poll_timeout_atomic` with delay 0 counts 1 ns per loop).
- A stuck kernel call that holds a driver lock also blocks `rmmod`, so a normal `reboot` hangs in the init scripts: `sync; reboot -f`. Replace the `.ko` first so the fixed driver loads at boot.
- Read the driver source of the DMA engine you use (here `drivers/dma/xilinx/xilinx_dma.c`) before designing a client: the API is generic, but each driver's behaviour is not.
- IIO: describe the hardware (channels, scan type, scale) and let the core make sysfs, `/dev/iio:deviceN` and the buffer; generic tools (`iio_info`, `iio_readdev`) work at once. Kernel helpers for DMA ADCs: `iio_dmaengine_buffer_setup()` + `iio_buffer_setup_ops`.
- Read the helper's source before trusting it with your DMA: the IIO DMA buffer uses two blocks for `read()`, capped at 64 KiB (`dma_get_max_seg_size`), and calls `dmaengine_terminate_sync()` on stop.
- With an S2MM DMA, stop the data source *before* the DMA is reset, and only when the DMA holds no buffer. Data that arrives after the reset stays in the DMA and breaks the next start (`has errors 10` = DMAIntErr, the next reader waits forever).
- A failing stop test needs two runs in a row: the first stream after a module load was fine; only the second one hung.
- A compatible string names the hardware, not the driver: two drivers can match one node; load only one.
- A reader stuck in `read()` keeps the device open: "Device or resource busy" on the next open, `rmmod` fails. `killall` it first.
- libiio 0.x on a mainline kernel prints "High-speed mode not enabled": harmless (ADI's out-of-tree mmap interface).

### Streaming data

- A streaming source that cannot wait (ADC) needs a FIFO big enough for the CPU's worst re-arm delay, and a ring with enough slack for the reader's worst pause. Bigger blocks = more slack and fewer interrupts.
- Put a sequence number in every sample: it proves "no loss" end to end, independent of the driver's counters. Make the counter wider than the largest block, or whole-block losses hide.
- Uncached (coherent) DMA memory: fine to copy in bulk, very slow to read word by word. Zero copy (mmap) only wins if the program does not read every sample from it; otherwise copy first (`read()`) or use cached buffers with explicit cache syncs.
- Measure before optimising: splitting the time with `-s`/`-m` showed the per-sample loop, not the copy, was the bottleneck.
- A network sender for a source that cannot wait needs slack for network pauses: on Linux, the TCP send buffer is that slack. The default (tens of KB) holds milliseconds; raise `net.core.wmem_max` and set `SO_SNDBUF`.
- TCP never loses data on the wire, but it can stall: a real-time source then loses data at the producer (driver overruns). Check both ends.
- A 16-bit sequence number hides losses of exact multiples of 65536 samples (16 blocks of 4096). Compare with the board's own counters, or use a wider counter.
- Wi-Fi reorders UDP datagrams: number them and sort before checking, or a swap looks like a huge loss.
- UDP sends a block as a burst at wire speed; a slower link (Wi-Fi) behind a router drops part of a big burst even at a low average rate. Smaller bursts or pacing fix it; TCP paces itself.
- Python on the laptop keeps up at tens of MB/s if each chunk or batch of datagrams is handled with numpy, never a Python loop per sample or per datagram.
- A laptop's UDP receive buffer is capped at `net.core.rmem_max` (~200 KB): `sudo sysctl -w net.core.rmem_max=8388608` for bursty streams.
