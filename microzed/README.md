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
| `board/post-image-dt.sh` | Buildroot post-image script: adds the `myled`, `axiregs@43c00000`, `pltimer@43c10000`, `dma@40400000`, `dmaloop` and `adcstream@43c20000` nodes to the `.dtb` before the SD image is made |
| `modules/axiregs/` | Project 02: platform driver for the AXI register block in the PL, sysfs files `reg0`, `reg1`, `sum`, `count`, `id` |
| `modules/pltimer/` | Project 03: PL timer driver, `/dev/pltimer` blocks until the next tick; sysfs `period_us`, `enable`, `ticks`, `pl_ticks`, `missed`, `stats` |
| `modules/dmaloop/` | Project 04: DMA engine client, loops a buffer through the PL (sysfs `size`, `run`, `result`) |
| `modules/adcstream/` | Project 05: simulated-ADC stream driver: DMA buffer ring, `/dev/adcstream` (blocks of samples), sysfs `enable`, `rate`, `freq`, `mode`, `block_samples`, `overruns`, `pl_dropped`, `status`, `snapshot` |
| `apps/adcread.c` | Project 05: reads `/dev/adcstream`, checks for lost samples, prints rate/min/max/mean per second, optional CSV; `-m` zero copy, `-s` skim |
| `overlay/usr/bin/adcstream-bench` | Project 05: runs `adcread` at rising sample rates to find the highest one with no loss |
| `overlay/srv/www/scope.html`, `overlay/srv/www/cgi-bin/adc` | Project 05: live chart of the stream, with rate/frequency/mode settings |
| `configs/linux.fragment` | Kernel options on top of `xilinx_zynq_defconfig` (`CONFIG_XILINX_DMA=m`) |
| `overlay/usr/bin/dmaloop-bench` | Runs the DMA loopback for sizes 1 KB to 4 MB and prints a throughput table |
| `modules/myled/` | Character driver: `/dev/myled` controls LED D3 (`echo 1`, `echo 0`, `echo t`, `cat`) |
| `overlay/` | Files copied into the board's root filesystem (mirrors `/`) |
| `overlay/etc/init.d/S90httpd` | Starts BusyBox httpd at boot |
| `overlay/etc/init.d/S30myled` | Loads the myled_irq driver at boot (`modprobe myled_irq`) |
| `overlay/etc/init.d/S40pl` | Programs the PL with `/lib/firmware/pl.bin`, loads axiregs and pltimer, then xilinx_dma + dmaloop (project 04 design) or xilinx_dma + adcstream (project 05 design) |
| `overlay/etc/network/interfaces` | eth0 DHCP with the fixed MAC `02:0a:35:00:00:01` |
| `overlay/srv/www/cgi-bin/` | Web page: `status`, `control` (buttons), `index.cgi` (redirect for `/`), `adc` (data for `/scope.html`) |
| `configs/busybox.fragment` | BusyBox options (httpd + CGI) |
| `Makefile` | Rebuild the packages in Buildroot, copy to the overlay, or push to the board |
| `br2-external/` | Project 06: Buildroot external tree with two packages, `microzed-modules` (our kernel modules, installed in `/lib/modules/<version>/microzed/`) and `microzed-apps` (`hello`, `blink`, `adcread`) |
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

## Buildroot packages (one-time setup)

Our kernel modules and programs are built by Buildroot, as packages in `br2-external/`. Buildroot builds the modules against its kernel, installs them in `/lib/modules/<kernel version>/microzed/` and runs `depmod`, so `modprobe myled_irq` works.

Point the Buildroot build at the external tree (run on the laptop, once; Buildroot remembers the path in `~/br-microzed/.config`):

```bash
cd ~/br-microzed && make BR2_EXTERNAL=$HOME/microzed-linux/microzed/br2-external menuconfig
```

In menuconfig: "External options" -> enable `microzed-apps` (it selects `microzed-modules` and libgpiod). Save, then:

```bash
cd ~/microzed-linux/microzed && make clean && make flash
```

`make clean` removes old hand-built `.o`/`.ko` files from `modules/*/`.

## Use

```bash
cd ~/microzed-linux/microzed
make            # rebuild the two packages in Buildroot (after editing modules/ or apps/)
make push       # also copy them to the running board (no re-flash)
make overlay    # copy overlay files and the bitstream into ~/mz-overlay
make flash      # everything, then write the SD card
```

Buildroot does not notice changes in `modules/` or `apps/`; `make` here runs `make microzed-modules-rebuild microzed-apps-rebuild` in `~/br-microzed`, which copies the sources again and rebuilds them.

After a kernel change (`make linux-rebuild`, new kernel config fragment), run `make` here too: Buildroot does not rebuild packages that depend on the kernel by itself. A full `make clean && make` in `~/br-microzed` rebuilds everything in the right order.

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

- The `microzed-modules` package installs `myled_irq.ko` in `/lib/modules/<kernel version>/microzed/`; `/etc/init.d/S30myled` loads it at boot with `modprobe myled_irq`.
- `make push` stops and removes leddaemon on the board, unloads any myled module, copies the modules, programs and scripts, and loads the driver.
- The web page writes to `/dev/myled` and reads the LED state from it.
- By hand: `/etc/init.d/S30myled start|stop|restart`, or `modprobe myled_irq` / `rmmod myled_irq`.

## AXI register block (project 02)

- Since project 03, the axiregs block is part of the project 03 bitstream (`mz03_pltimer.bin`), loaded by `/etc/init.d/S40pl`. The project 02 bitstream (`fpga/02-axiregs/build.tcl`, `axiregs.bin`) is no longer installed.
- The `microzed-modules` package builds and installs `axiregs.ko` (like all our modules).
- The driver needs the `axiregs@43c00000` device tree node: `make flash` after pulling (the post-image script adds it).

On the board:

```bash
cd /sys/bus/platform/devices/43c00000.axiregs
echo 5 > reg0; echo 0x10 > reg1; cat sum     # 21
cat count; cat count                          # rises by ~100 million per second
cat id                                        # 0x4d5a0201
```

Never read `0x43C00000` (devmem or driver) before the bitstream is loaded: the CPU hangs until a power cycle.

## PL timer (project 03)

- `fpga/03-pltimer/build.tcl` makes `~/vivado/mz03_pltimer/mz03_pltimer.bin` (axiregs + pltimer, see `fpga/README.md`).
- `make overlay` / `make push` install the bitstream as `/lib/firmware/pl.bin` (the newest built design).
- At boot, `/etc/init.d/S40pl` programs the PL, then loads axiregs and pltimer. By hand: `/etc/init.d/S40pl start|stop|restart`.
- The driver needs the `pltimer@43c10000` node (with `interrupts = <0 29 4>`): `make flash` after pulling.

On the board:

```bash
cat /dev/pltimer                      # one line per tick (default every 1 s), Ctrl-C to stop
cd /sys/bus/platform/devices/43c10000.pltimer
echo 100000 > period_us               # 100 ms
cat ticks pl_ticks missed stats
grep pltimer /proc/interrupts
```


## AXI DMA loopback (project 04)

- One-time: Buildroot menuconfig -> Kernel -> "Additional configuration fragment files" = `/home/fabio/microzed-linux/microzed/configs/linux.fragment`, then `make linux-reconfigure && make` in `~/br-microzed`. It makes the Xilinx DMA driver a module: built in, it would hang the boot (it reads the DMA registers before the PL is programmed).
- `post-image-dt.sh` writes the DMA nodes with `status = "okay"` only if the kernel has `CONFIG_XILINX_DMA=m`; otherwise disabled, with a warning.
- `fpga/04-dma-loopback/build.tcl` builds the bitstream (project 03 design + DMA); `make flash` installs everything.
- At boot `S40pl` loads `pl.bin`, axiregs, pltimer, then (axiregs ID `0x4d5a04xx` only) `xilinx_dma` and `dmaloop`.

On the board:

```bash
cd /sys/bus/platform/devices/dmaloop
echo 65536 > size; echo 100 > run; cat result
dmaloop-bench          # table for 1 KB .. 4 MB
```

## DMA data stream, simulated ADC (project 05)

- `fpga/05-adc-stream/build.tcl` builds `~/vivado/mz05_adc/mz05_adc.bin` (project 04 design, with `adcsim` at `0x43C20000` feeding the DMA's S2MM side). Once built, `make overlay` / `make push` install it as `/lib/firmware/pl.bin`.
- The driver needs the `adcstream@43c20000` node: `make flash` after pulling.
- At boot `S40pl` loads the bitstream, axiregs, pltimer, then (axiregs ID `0x4d5a05xx` only) `xilinx_dma` and `adcstream`. The stream starts at load: 1 MSa/s, 1 kHz sine, blocks of 4096 samples.

On the board:

```bash
cd /sys/bus/platform/devices/43c20000.adcstream
cat status
adcread -t 60                 # 60 s, one line per second, summary at the end
echo 5000000 > rate           # 5 MSa/s (100 MHz / 20)
echo 0 > enable; echo 16384 > block_samples; echo 1 > enable
adcstream-bench 10 32768      # highest rate with no loss
adcstream-bench 10 32768 -m -s   # zero copy, no per-sample work
```

Live chart: `http://10.0.0.134/scope.html` (also linked from the status page).
