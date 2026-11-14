# Buildroot + QEMU + MicroZed: project summary

Last updated: 2026-11-14

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

As of 2026-11-14 (afternoon): **GPIO works and SSH works.**

1. `~/br-microzed/.config` has: `dropbear`, **libgpiod v1** (`BR2_PACKAGE_LIBGPIOD=y`, `BR2_PACKAGE_LIBGPIOD_TOOLS=y`), a root password, and `BR2_SYSTEM_DHCP="eth0"`.
2. SD card re-flashed with this image. `/etc/network/interfaces` now has `auto eth0` / `iface eth0 inet dhcp`.
3. GPIO: `gpiodetect` shows `gpiochip0 [zynq_gpio] (118 lines)` (lines 0-53 = MIO, 54-117 = EMIO). LED D3 and button PB1 both work (commands below).
4. SSH: works with `ssh root@<ip>` (got `10.0.0.14` once; the IP can change because the MAC is random each boot). `uname -m` on the board shows `armv7l`.
5. **Known board issue:** the Ethernet PHY is often not detected at power-up (details and workaround below).

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

**Workaround that works:** power up **without the Ethernet cable plugged in**. Plug the cable in after boot, then press SW2 (reset) if needed. SSH then works.

Likely cause (not confirmed): the 88E1512 reads its address and mode from strap pins at power-up, and some are shared with the RJ45 LED lines. With a live link partner connected at power-up, the straps may be read wrong. To confirm: check the PHY strap/CONFIG and reset wiring in the Avnet user guide or schematic, and test the factory image's cold-boot success rate.

## Next steps

1. Set a fixed MAC in U-Boot so the IP stays the same: stop at the U-Boot countdown, then `setenv ethaddr 02:0a:35:00:00:01` and `saveenv`. Optionally reserve the IP in the router.
2. Cross-compile `hello.c` with `~/br-microzed/host/bin/arm-buildroot-linux-gnueabihf-gcc` (check the exact name), then `scp -O` it to the board (N9). Dropbear has no sftp server, hence `-O`.
3. Set up a rootfs overlay (`BR2_ROOTFS_OVERLAY`) for custom files and scripts, so they survive re-flashing.
4. Later: look into the PHY strap/reset wiring for a proper fix.

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
