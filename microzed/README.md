# MicroZed files

Sources for the MicroZed board: kernel modules, programs, the rootfs overlay, the FPGA (PL) designs and the laptop tools. Background, results and lessons learned are in `../buildroot-microzed-project.md`.

```
microzed/
  Makefile        make / make push / make overlay / make flash
  flash-sd.sh     writes the Buildroot image to the SD card (safety checks)
  apps/           programs for the board (Buildroot package microzed-apps)
  modules/        kernel modules (Buildroot package microzed-modules)
  br2-external/   the Buildroot external tree that defines those two packages
  board/          post-image script: adds our device tree nodes to the .dtb
  configs/        kernel and BusyBox config fragments
  overlay/        files copied into the board's root filesystem (mirrors /)
  fpga/           PL sources: Verilog, testbenches, Vivado Tcl (see fpga/README.md)
  tools/          programs that run on the laptop
  projects/       one brief per project (see projects/README.md)
  docs/           Avnet MicroZed user guide (PDF)
```

## What is in each folder

**apps/** (installed in `/usr/bin`)
- `hello.c`: cross-compile test.
- `blink.c`: blinks LED D3 until PB1 is pressed (libgpiod v1; `rmmod myled_irq` first).
- `adcread.c`: reads the ADC stream and checks every sample for loss. `/dev/adcstream` by default, `-i` the IIO device, `-f file` a recording; `-m` zero copy, `-s` skim, `-c` CSV.
- `adcserve.c`: sends the ADC stream to the laptop over TCP (default) or UDP (`-u`), port 5000.

**modules/** (installed in `/lib/modules/<version>/microzed/`, load with `modprobe`)
- `hello/`: minimal module, a template for quick experiments (not in the package).
- `myled-irq/`: LED D3 and button PB1. `/dev/myled` (write `1`, `0`, `t`), `/dev/mybutton` (blocks until a press).
- `axiregs/`: project 02, AXI register block (sysfs `reg0`, `reg1`, `sum`, `count`, `id`).
- `pltimer/`: project 03, PL timer interrupt, `/dev/pltimer` blocks until the next tick.
- `dmaloop/`: project 04, AXI DMA loopback test (sysfs `size`, `run`, `result`).
- `adcstream/`: project 05, simulated ADC stream, `/dev/adcstream` (read or mmap), sysfs settings.
- `adciio/`: project 07, the same ADC as a standard IIO device `adcsim`.

Each module folder also has its own Makefile, to build and copy one module by hand (`make`, `make push`, then `insmod /root/<name>.ko` on the board).

**overlay/**
- `etc/init.d/S30myled`: loads myled_irq at boot.
- `etc/init.d/S40pl` + `etc/default/pl`: programs the PL with `/lib/firmware/pl.bin`, then loads the drivers for that design. `ADC_DRIVER=adcstream` (default) or `adciio`.
- `etc/init.d/S90httpd`: BusyBox web server.
- `etc/init.d/S95adcserve` + `etc/default/adcserve`: TCP and UDP sample servers (`ADCSERVE=no` turns them off).
- `etc/network/interfaces`: eth0 DHCP, fixed MAC `02:0a:35:00:00:01` (board IP 10.0.0.134).
- `srv/www/`: status page (`cgi-bin/status`, `control`, `index.cgi`) and live ADC chart (`scope.html`, `cgi-bin/adc`).
- `usr/bin/*-bench`: highest lossless rate tests (`dmaloop-bench`, `adcstream-bench`, `adciio-bench`).

**tools/** (laptop, Python 3 + numpy, matplotlib for `--plot`)
- `adcrecv.py`: receives from adcserve, checks for loss, records (`-o`), live plot + FFT (`--plot`).
- `adcnet-bench`: highest lossless rate over the network (sets each rate over ssh).
- `iio_check.py`: checks a recording (`iio_readdev` or `adcrecv.py -o`): loss, frequency, sine fit.

Not in git: the board's SSH host keys (`~/mz-overlay/etc/dropbear/`) and `~/mz-overlay/root/.ssh/`.

## Setup (once, on the laptop)

```bash
git clone https://github.com/Fuzzyvolt/microzed-linux.git ~/microzed-linux
```

Point Buildroot at the external tree. In menuconfig, "External options": enable `microzed-apps` (it selects `microzed-modules` and libgpiod):

```bash
cd ~/br-microzed && make BR2_EXTERNAL=$HOME/microzed-linux/microzed/br2-external menuconfig
```

Other Buildroot settings this repo needs (all already done, details in the project md):
- Kernel -> "Additional configuration fragment files": `/home/fabio/microzed-linux/microzed/configs/linux.fragment`
- BusyBox -> "Additional BusyBox configuration fragment files": `/home/fabio/mz-busybox.fragment` (same content as `configs/busybox.fragment`)
- System configuration -> post-image scripts: `/home/fabio/microzed-linux/microzed/board/post-image-dt.sh` **before** `board/zynq/post-image.sh`
- `BR2_ROOTFS_OVERLAY="/home/fabio/mz-overlay"`; Target packages: dropbear, libgpiod (v1) with tools, ethtool, libiio with test programs

For key login (no password prompts during `make push`):

```bash
ssh-copy-id root@10.0.0.134
```

## Day to day

Get the latest files:

```bash
cd ~/microzed-linux && git pull
```

Then, in `~/microzed-linux/microzed`:

```bash
make            # rebuild both packages in Buildroot
make push       # also copy everything to the running board (no re-flash)
make overlay    # copy overlay files and the bitstream into ~/mz-overlay
make flash      # everything, then write the SD card (SD=/dev/sdc if not sdb)
```

- `make push` is enough for module, program and script changes.
- `make flash` is needed for device tree, kernel, BusyBox or Buildroot changes, and to make changes permanent.
- Buildroot does not notice edits in `modules/` or `apps/`; `make` here runs `make microzed-modules-rebuild microzed-apps-rebuild` in `~/br-microzed`.
- After a kernel change, run `make` here too.
- The bitstream comes from `~/vivado/` (built with `fpga/*/build.tcl`). The newest built design is installed as `/lib/firmware/pl.bin`: project 05, else 04, else 03.

## Try it on the board

All commands run on the board (`ssh root@10.0.0.134`) unless marked laptop.

LED and button (myled_irq):

```bash
echo t > /dev/myled; cat /dev/mybutton
```

AXI registers (project 02):

```bash
cd /sys/bus/platform/devices/43c00000.axiregs && echo 5 > reg0 && echo 16 > reg1 && cat sum id
```

PL timer (project 03):

```bash
cat /dev/pltimer
```

DMA loopback (project 04, needs the project 04 bitstream):

```bash
dmaloop-bench
```

ADC stream (project 05), chart at `http://10.0.0.134/scope.html`:

```bash
adcread -t 10
```

ADC as an IIO device (project 07):

```bash
/etc/init.d/S40pl stop; ADC_DRIVER=adciio /etc/init.d/S40pl start; iio_readdev -b 8192 -s 1000000 adcsim > /tmp/adc.bin; adcread -f /tmp/adc.bin
```

Stream to the laptop (project 08), run on the laptop:

```bash
cd ~/microzed-linux/microzed/tools && python3 adcrecv.py --plot
```

Never read a PL address (`devmem` or a driver) before the bitstream is loaded: the CPU hangs until a power cycle. `S40pl` and the drivers check this.
