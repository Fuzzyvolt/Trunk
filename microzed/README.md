# MicroZed files

Source for the programs and the Buildroot rootfs overlay used in `buildroot-microzed-project.md`.

## Layout

| Path | What it is |
|---|---|
| `apps/hello.c` | Cross-compile test |
| `apps/blink.c` | Blinks LED D3 until PB1 is pressed (libgpiod v1) |
| `apps/leddaemon.c`, `apps/leddaemon.init` | Retired user-space daemon for LED D3 and PB1 (replaced by the myled_irq driver); kept for reference |
| `modules/hello/` | First kernel module (own Makefile, builds against the Buildroot kernel) |
| `modules/myled-dt/` | Same `/dev/myled`, as a platform driver bound to a device tree node (`compatible = "microzed,myled"`) |
| `modules/myled-irq/` | Adds PB1 with an interrupt: `/dev/mybutton` blocks until a press; a press toggles the LED |
| `board/post-image-dt.sh` | Buildroot post-image script: adds the `myled` and `axiregs@43c00000` nodes to the `.dtb` before the SD image is made |
| `modules/axiregs/` | Project 02: platform driver for the AXI register block in the PL, sysfs files `reg0`, `reg1`, `sum`, `count`, `id` |
| `modules/myled/` | Character driver: `/dev/myled` controls LED D3 (`echo 1`, `echo 0`, `echo t`, `cat`) |
| `overlay/` | Files copied into the board's root filesystem (mirrors `/`) |
| `overlay/etc/init.d/S90httpd` | Starts BusyBox httpd at boot |
| `overlay/etc/init.d/S30myled` | Loads the myled_irq driver at boot (`/lib/modules/myled/myled_irq.ko`) |
| `overlay/etc/init.d/S40axiregs` | Programs the PL with `/lib/firmware/axiregs.bin`, then loads the axiregs driver |
| `overlay/etc/network/interfaces` | eth0 DHCP with the fixed MAC `02:0a:35:00:00:01` |
| `overlay/srv/www/cgi-bin/` | Web page: `status`, `control` (buttons), `index.cgi` (redirect for `/`) |
| `configs/busybox.fragment` | BusyBox options (httpd + CGI) |
| `Makefile` | Build, copy to the overlay, or push to the board |
| `fpga/` | FPGA (PL) sources: Verilog, Vivado Tcl scripts; projects are built outside the repo |
| `projects/` | Briefs for the next projects (FPGA side), one per session |

Not in git: the SSH host keys in `~/mz-overlay/etc/dropbear/` (private).

## Get the files on the Mint laptop

First time:

```bash
git clone https://github.com/Fuzzyvolt/microzed-linux.git ~/microzed-linux
```

After that, to get updates:

```bash
cd ~/microzed-linux && git pull
```

## Use

```bash
cd ~/microzed-linux/microzed
make            # build the programs
make push       # test on the running board right away (no re-flash)
make overlay    # copy into ~/mz-overlay for the next image
```

`make push` asks for the root password several times unless you set up a key with `ssh-copy-id root@10.0.0.134`.

## LED daemon (retired)

Replaced by the myled_irq kernel driver, which is loaded at boot. To use the daemon again, copy `apps/leddaemon.init` to `/etc/init.d/S99leddaemon` and unload the driver first.


- Commands: `echo on > /tmp/led.fifo` (also `off`, `toggle`).
- PB1 toggles the LED.
- State for the web page: `/tmp/led.state`.
- Logs: `logread | grep leddaemon` (or `/var/log/messages`).
- Debug in the foreground: stop the service, then run `leddaemon -f`.
- `blink` and `gpioset` on lines 47/51 fail with "Device or resource busy" while the daemon runs. Stop it first.

## Flash the SD card in one step

```bash
make flash
```

This copies the overlay, rebuilds the Buildroot image, then runs `flash-sd.sh`: it checks that `/dev/sdb` is a removable card (never the PC's own disk), shows it, asks you to type `yes`, unmounts, writes with `dd`, syncs and ejects. Use `make flash SD=/dev/sdc` if the card has another name, or `./flash-sd.sh` to flash the existing image without rebuilding.

## Kernel modules

Each module has its own folder and Makefile, which builds against the kernel tree Buildroot built (`~/br-microzed/build/linux-*`).

```bash
cd ~/microzed-linux/microzed/modules/hello
make            # builds hello.ko
make push       # copies it to the board's /root/
```

On the board:

```bash
insmod /root/hello.ko
dmesg | tail -3
rmmod hello
```

A module only loads into the exact kernel it was built against. After a kernel change in Buildroot, rebuild the module.

### myled (character driver)

```bash
cd ~/microzed-linux/microzed/modules/myled && make push
```

On the board (stop leddaemon first, only one user of the LED line at a time):

```bash
/etc/init.d/S99leddaemon stop
insmod /root/myled.ko
echo 1 > /dev/myled
cat /dev/myled
rmmod myled
/etc/init.d/S99leddaemon start
```

### myled-dt (device tree + platform driver)

One-time setup on the laptop:

```bash
sudo apt install device-tree-compiler
cd ~/br-microzed && make menuconfig
```

System configuration -> "Custom scripts to run after creating filesystem images": put
`/home/fabio/microzed-linux/microzed/board/post-image-dt.sh` in front of what is already there (keep the genimage script after it). Then `make flash`.

On the board: check the node with `ls /proc/device-tree/myled`, then load the driver (stop leddaemon and `rmmod myled` first):

```bash
insmod /root/myled_dt.ko
ls /sys/bus/platform/drivers/myled/
echo 1 > /dev/myled
```

### myled-irq (button interrupt)

Needs `button-gpios` in the node: re-run `make flash` after pulling (the post-image script adds it).

```bash
cd ~/microzed-linux/microzed/modules/myled-irq && make push
```

On the board (stop leddaemon, `rmmod` any other myled module first):

```bash
insmod /root/myled_irq.ko
cat /dev/mybutton            # prints "press N" on every PB1 press, Ctrl-C to stop
grep myled /proc/interrupts  # interrupt count
```

## Driver at boot (current setup)

- `make` also builds `modules/myled-irq/myled_irq.ko`.
- `make overlay` / `make flash` install it as `/lib/modules/myled/myled_irq.ko`; `/etc/init.d/S30myled` loads it at boot with `insmod`.
- `make push` stops and removes leddaemon on the board, unloads any myled module, copies the module, programs and scripts, and loads the driver.
- The web page writes to `/dev/myled` and reads the LED state from it.
- By hand: `/etc/init.d/S30myled start|stop|restart`.
- After a kernel change in Buildroot, rebuild the module (`make`): a module built for another kernel fails to load ("invalid module format").

## AXI register block (project 02)

- `fpga/02-axiregs/build.tcl` makes `~/vivado/mz02_axiregs/axiregs.bin` (see `fpga/README.md`).
- `make` builds `modules/axiregs/axiregs.ko`; `make overlay` / `make push` install it as `/lib/modules/axiregs/axiregs.ko` and the bitstream as `/lib/firmware/axiregs.bin` (skipped with a warning if it is not built yet).
- At boot, `/etc/init.d/S40axiregs` programs the PL, then loads the driver. By hand: `/etc/init.d/S40axiregs start|stop|restart`.
- The driver needs the `axiregs@43c00000` device tree node: `make flash` after pulling (the post-image script adds it).

On the board:

```bash
cd /sys/bus/platform/devices/43c00000.axiregs
echo 5 > reg0; echo 0x10 > reg1; cat sum     # 21
cat count; cat count                          # rises by ~100 million per second
cat id                                        # 0x4d5a0201
```

Never read `0x43C00000` (devmem or driver) before the bitstream is loaded: the CPU hangs until a power cycle.
