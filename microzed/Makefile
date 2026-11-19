# MicroZed apps, modules and overlay files.
#
# Our kernel modules and programs are Buildroot packages (br2-external/:
# microzed-modules, microzed-apps). This Makefile asks Buildroot to rebuild
# them from the repo sources, then copies the results.
#
#   make            rebuild both packages in Buildroot (modules + programs)
#   make push       make, then copy modules, programs and scripts to the running board
#   make overlay    copy overlay files (scripts, web page, bitstream) into ~/mz-overlay
#   make image      make overlay + make, then rebuild the Buildroot image
#   make flash      make image, then write it to the SD card (default /dev/sdb, override with SD=/dev/sdc)
#
# One-time setup (see README): point the Buildroot build at br2-external/ and
# enable the packages:
#   cd ~/br-microzed && make BR2_EXTERNAL=$HOME/microzed-linux/microzed/br2-external menuconfig
#
# Override paths on the command line, for example: make push BOARD=10.0.0.135
#
# The PL bitstream is built by Vivado outside the repo (fpga/*/build.tcl).
# FPGA_BIN is the newest design that is built: project 05 (simulated ADC
# stream), else 04 (DMA loopback), else 03. It is installed on the board as
# /lib/firmware/pl.bin, which /etc/init.d/S40pl loads. If none exists, it is skipped.

BR      ?= $(HOME)/br-microzed
OVERLAY ?= $(HOME)/mz-overlay
BOARD   ?= 10.0.0.134
SD      ?= /dev/sdb
FPGA_BIN ?= $(firstword $(wildcard $(HOME)/vivado/mz05_adc/mz05_adc.bin \
                                  $(HOME)/vivado/mz04_dma/mz04_dma.bin) \
                        $(HOME)/vivado/mz03_pltimer/mz03_pltimer.bin)

PKGS    := microzed-modules microzed-apps
APPS    := hello blink adcread adcserve

# Kernel version, like 6.12.0-xilinx: modules live in /lib/modules/$(KVER)/
KVER     = $(shell cat $(firstword $(wildcard $(BR)/build/linux-*/include/config/kernel.release)) 2>/dev/null)
MODDIR   = /lib/modules/$(KVER)
DEPMOD  := $(BR)/host/sbin/depmod

# Files from before the packages (and older retired ones). The overlay only
# adds files, so they are removed from the overlay and from Buildroot's
# target/ by hand. Old program copies in the overlay would even replace the
# package's newer ones, because the overlay is copied last.
OLD_MODDIRS := myled axiregs pltimer dmaloop adcstream
OLD_FILES   := etc/init.d/S99leddaemon etc/init.d/S40axiregs \
               lib/firmware/axiregs.bin lib/firmware/mz03_pltimer.bin \
               usr/bin/leddaemon
OLD_OVERLAY := $(OLD_FILES) $(addprefix usr/bin/,$(APPS))

all: packages

# Buildroot does not notice changes in local sources: -rebuild copies them
# again (rsync) and rebuilds. microzed-apps comes second (it needs adcstream.h
# from microzed-modules).
packages:
	$(MAKE) -C $(BR) $(addsuffix -rebuild,$(PKGS))

overlay:
	cp -a overlay/. $(OVERLAY)/
	@if [ -f $(FPGA_BIN) ]; then install -D -m 644 $(FPGA_BIN) $(OVERLAY)/lib/firmware/pl.bin; \
	echo "Bitstream: $(FPGA_BIN) -> /lib/firmware/pl.bin"; \
	else echo "WARNING: $(FPGA_BIN) not found, bitstream not copied (run fpga/05-adc-stream/build.tcl)"; fi
	rm -f $(addprefix $(OVERLAY)/,$(OLD_OVERLAY)) $(addprefix $(BR)/target/,$(OLD_FILES))
	rm -rf $(addprefix $(OVERLAY)/lib/modules/,$(OLD_MODDIRS)) $(addprefix $(BR)/target/lib/modules/,$(OLD_MODDIRS))
	@echo "Copied into $(OVERLAY). Now run 'make' in $(BR) and re-flash."

# Buildroot runs depmod only in its final step (a full 'make' in $(BR)), so
# push runs the same host depmod on target/ itself, then copies our modules
# and the index files (modules.dep, modules.alias...). The board's BusyBox
# has modprobe but no depmod.
push: packages
	@test -n "$(KVER)" || { echo "Kernel version not found in $(BR)/build/linux-*"; exit 1; }
	$(DEPMOD) -a -b $(BR)/target $(KVER)
	-ssh root@$(BOARD) '[ -x /etc/init.d/S95adcserve ] && /etc/init.d/S95adcserve stop; /etc/init.d/S99leddaemon stop 2>/dev/null; /etc/init.d/S30myled stop 2>/dev/null; rmmod myled_irq myled_dt myled 2>/dev/null; true'
	-ssh root@$(BOARD) 'for s in S40axiregs S40pl; do [ -x /etc/init.d/$$s ] && /etc/init.d/$$s stop; done; rmmod adciio adcstream dmaloop xilinx_dma pltimer axiregs 2>/dev/null; true'
	ssh root@$(BOARD) 'cd / && rm -f $(OLD_FILES) && cd /lib/modules && rm -rf $(OLD_MODDIRS) && mkdir -p $(MODDIR)/microzed /lib/firmware'
	scp -O $(BR)/target$(MODDIR)/microzed/*.ko root@$(BOARD):$(MODDIR)/microzed/
	scp -O $(BR)/target$(MODDIR)/modules.* root@$(BOARD):$(MODDIR)/
	@if [ -f $(FPGA_BIN) ]; then scp -O $(FPGA_BIN) root@$(BOARD):/lib/firmware/pl.bin; \
	else echo "WARNING: $(FPGA_BIN) not found, bitstream not copied"; fi
	scp -O $(addprefix $(BR)/target/usr/bin/,$(APPS)) overlay/usr/bin/* root@$(BOARD):/usr/bin/
	scp -O overlay/srv/www/cgi-bin/* root@$(BOARD):/srv/www/cgi-bin/
	scp -O overlay/srv/www/scope.html root@$(BOARD):/srv/www/
	scp -O overlay/etc/init.d/* root@$(BOARD):/etc/init.d/
	ssh root@$(BOARD) 'mkdir -p /etc/default'
	scp -O overlay/etc/default/* root@$(BOARD):/etc/default/
	ssh root@$(BOARD) 'rm -f /srv/www/index.html; /etc/init.d/S30myled start; /etc/init.d/S40pl start; /etc/init.d/S95adcserve start'

image: overlay packages
	$(MAKE) -C $(BR)

flash: image
	./flash-sd.sh $(SD)

# Leftovers from the old hand-made builds (build/ and modules/*/). The
# per-module Makefiles still work for quick experiments.
clean:
	rm -rf build
	cd modules && rm -f */*.o */*.ko */*.mod */*.mod.c */.*.cmd */modules.order */Module.symvers

.PHONY: all packages overlay push image flash clean
