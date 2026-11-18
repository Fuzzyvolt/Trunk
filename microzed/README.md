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
| `apps/adcread.c` | Project 05: reads `/dev/adcstream`, checks for lost samples, prints rate/min/max/mean per second, optional CSV |
| `overlay/usr/bin/adcstream-bench` | Project 05: runs `adcread` at rising sample rates to find the highest one with no loss |
| `overlay/srv/www/scope.html`, `overlay/srv/www/cgi-bin/adc` | Project 05: live chart of the stream, with rate/frequency/mode settings |
| `configs/linux.fragment` | Kernel options on top of `xilinx_zynq_defconfig` (`CONFIG_XILINX_DMA=m`) |
| `overlay/usr/bin/dmaloop-bench` | Runs the DMA loopback for sizes 1 KB to 4 MB and prints a throughput table |
| `modules/myled/` | Character driver: `/dev/myled` controls LED D3 (`echo 1`, `echo 0`, `echo t`, `cat`) |
| `overlay/` | Files copied into the board's root filesystem (mirrors `/`) |
| `overlay/etc/init.d/S90httpd` | Starts BusyBox httpd at boot |
| `overlay/etc/init.d/S30myled` | Loads the myled_irq driver at boot (`/lib/modules/myled/myled_irq.ko`) |
| `overlay/etc/init.d/S40pl` | Programs the PL with `/lib/firmware/pl.bin`, loads axiregs and pltimer, then xilinx_dma + dmaloop (project 04 design) or xilinx_dma + adcstream (project 05 design) |
| `overlay/etc/network/interfaces` | eth0 DHCP with the fixed MAC `02:0a:35:00:00:01` |
| `overlay/srv/www/cgi-bin/` | Web page: `status`, `control` (buttons), `index.cgi` (redirect for `/`), `adc` (data for `/scope.html`) |
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

- Since project 03, the axiregs block is part of the project 03 bitstream (`mz03_pltimer.bin`), loaded by `/etc/init.d/S40pl`. The project 02 bitstream (`fpga/02-axiregs/build.tcl`, `axiregs.bin`) is no longer installed.
- `make` builds `modules/axiregs/axiregs.ko`; `make overlay` / `make push` install it as `/lib/modules/axiregs/axiregs.ko`.
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
- `make` builds `modules/pltimer/pltimer.ko`; `make overlay` / `make push` install it as `/lib/modules/pltimer/pltimer.ko` and the bitstream as `/lib/firmware/pl.bin` (project 04's, once built).
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
adcstream-bench 10            # highest rate with no loss
```

Live chart: `http://10.0.0.134/scope.html` (also linked from the status page).
