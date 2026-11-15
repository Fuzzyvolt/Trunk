# Buildroot + QEMU + MicroZed: project summary

Last updated: 2026-11-15

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

As of 2026-11-14 (night): **GPIO, SSH (key login), cross-compiled programs, rootfs overlay, a status web page with LED/power-off buttons, and an LED daemon all work.** Work has moved to a **second MicroZed**, because the first board's Ethernet PHY is unreliable (see below). Kernel modules `hello`, `myled` (character driver) and `myled-dt` (device tree platform driver) also work.

1. `~/br-microzed/.config` has: `dropbear`, **libgpiod v1** (`BR2_PACKAGE_LIBGPIOD=y`, `BR2_PACKAGE_LIBGPIOD_TOOLS=y`), **ethtool**, a root password, `BR2_SYSTEM_DHCP="eth0"`, `BR2_ROOTFS_OVERLAY="/home/fabio/mz-overlay"`, and the BusyBox fragment `/home/fabio/mz-busybox.fragment`.
2. GPIO: `gpiodetect` shows `gpiochip0 [zynq_gpio] (118 lines)` (lines 0-53 = MIO, 54-117 = EMIO). LED D3 and button PB1 are owned by `leddaemon`.
3. Network: fixed MAC `02:0a:35:00:00:01`, so the board gets **10.0.0.134**. `ssh root@10.0.0.134` logs in with a key (no password).
4. Web page: `http://10.0.0.134` (redirects to `/cgi-bin/status`): die temperature, all XADC voltage rails, uptime, load, memory, IP, kernel, LED state, and buttons (LED on/off/toggle, power off). Refreshes every 5 s.
5. Config backup: `~/mz-overlay-microzed.config` (copy of `.config`).

### Repo workflow (`~/trunk/microzed`)

All source files now live in the git repo, in `microzed/` (see `microzed/README.md`). Claude writes files there; the laptop gets them with git.

- Laptop clone: `~/trunk`, branch `claude/micro-zed-project-ideas-o0fk5t` (private repo: git asks for the GitHub username and a personal access token). Update with `cd ~/trunk && git pull`.
- `cd ~/trunk/microzed`, then:
  - `make`: cross-compile the programs into `build/`.
  - `make push`: stop leddaemon, copy programs and scripts to the running board over SSH, start leddaemon again. No re-flash needed for programs and scripts.
  - `make overlay`: copy programs and overlay files into `~/mz-overlay`.
  - `make flash`: `make overlay`, rebuild Buildroot, then `flash-sd.sh` (checks the card is removable and not the PC's disk, asks for `yes`, unmounts, `dd`, sync, eject). `make flash SD=/dev/sdc` for another device.
- Use `make push` for program/script changes, `make flash` for Buildroot changes (packages, kernel, BusyBox) or to make changes permanent.
- Kernel modules: `microzed/modules/<name>/`, built with their own Makefile (`make`, `make push`).

### SSH key login

- Key made on the laptop with `ssh-keygen -t ed25519`, copied with `ssh-copy-id root@10.0.0.134` (run on the laptop, not on the board).
- To keep it after re-flashing, it is in the overlay: `~/mz-overlay/root/.ssh/authorized_keys` (folder 700, file 600; Dropbear ignores the key if permissions are looser).
- `~/.ssh/id_ed25519` (no `.pub`) is the private key: keep it on the laptop only.

### LED daemon (`microzed/apps/leddaemon.c`)

- Started at boot by `/etc/init.d/S99leddaemon` (BusyBox `start-stop-daemon`, pid file `/var/run/leddaemon.pid`).
- Owns LED D3 (output) and PB1 (both-edge events, so it sleeps until the button interrupt fires). Debounce: after an edge, wait until the line is quiet for 30 ms, then read the settled value; a 0 -> 1 change toggles the LED.
- Commands through a FIFO: `echo on|off|toggle > /tmp/led.fifo`. The FIFO is opened read-write, so it never reports end-of-file when a writer closes it.
- Writes the state to `/tmp/led.state` (temp file + rename, so readers never see half a file). Logs to syslog: `logread | grep leddaemon`. Foreground debug: stop the service, run `leddaemon -f`.
- The web page's `control` CGI only accepts POST with exact values (`led_on`, `led_off`, `led_toggle`, `poweroff`), sends commands to the FIFO only if the daemon is running (a FIFO with no reader would block forever), and redirects back with `303 See Other`. Power off asks for confirmation in the browser.
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
- Buildroot setting: `BR2_ROOTFS_POST_IMAGE_SCRIPT="/home/fabio/trunk/microzed/board/post-image-dt.sh board/zynq/post-image.sh"`. **Order matters**: our script must run before `board/zynq/post-image.sh`, which runs genimage to make `sdcard.img`.
- Check before flashing: `fdtget ~/br-microzed/images/*.dtb /myled compatible`. On the board: `ls /proc/device-tree/myled/`; the platform device exists even before the driver loads (`/sys/bus/platform/devices/`).
- The driver only registers itself in init; the kernel calls `probe()` for each node whose `compatible` matches `of_match_table`. `probe()` reads `label` (`device_property_read_string`) and `led-gpios` (`devm_gpiod_get(dev, "led", ...)`), then creates `/dev/myled`. `devm_*` resources are freed automatically on unbind. Binding shows in `/sys/bus/platform/drivers/myled/`.
- `platform_driver.remove` returns void from kernel 6.11 (int before), handled with `LINUX_VERSION_CODE`.

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

1. Point the web page's `control` script at `/dev/myled`.
2. Load the driver at boot; add the button with an interrupt handler and a blocking `read()`.
3. Watch the pause counters during the next Wi-Fi disruption.
4. PHY test on board 2: 5 cold boots with the Ethernet cable plugged in; tally good/bad.

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
- Copying over a running program fails with "Text file busy": stop it first (`make push` does this for leddaemon).
