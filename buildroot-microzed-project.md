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

1. Added to `~/br-microzed/.config`: `dropbear`, `libgpiod2` + "install tools", a root password, DHCP on `eth0`. These were first set in the QEMU folder by mistake, then copied over.
2. Rebuilt and re-flashed the SD card. The board boots, asks for the root password, and `dropbear` is present (`/usr/sbin/dropbear`).
3. **Open problem:** `gpiodetect` is not found on the board, so the libgpiod2 tools did not make it into the image.
   - Suspected cause: the libgpiod2 option was silently dropped by `make olddefconfig` or a menuconfig dependency (libgpiod2 needs kernel headers >= 5.10), or "install tools" is off.
   - Next checks on the PC:
     ```bash
     grep -E "LIBGPIOD|HEADERS_AT_LEAST_5_10=|BR2_LINUX_KERNEL_VERSION|KERNEL_HEADERS" ~/br-microzed/.config
     ls ~/br-microzed/target/usr/bin/ | grep gpio
     ```
     If the option is missing: in `make menuconfig`, press `/`, search `LIBGPIOD2`, and read its "Depends on" line.
   - Workaround meanwhile: sysfs GPIO, if `/sys/class/gpio` exists on the board (pin number = gpiochip base + MIO number).
4. Ethernet is not connected yet. SSH has not been tried.

## Next steps

1. Fix the libgpiod2 tools, then drive LED D3 and read button PB1 (blocks N3-N5 in the command file).
2. Connect Ethernet, get the IP with `ip addr show eth0`, and `ssh root@<ip>` from the PC (N7-N8).
3. Cross-compile `hello.c` with `~/br-microzed/host/bin/arm-buildroot-linux-gnueabihf-gcc` (check the exact name), then `scp -O` it to the board (N9). Dropbear has no sftp server, hence `-O`.

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
