# MicroZed apps and overlay files.
#
#   make            build the programs (into build/) and the kernel modules (myled_irq, axiregs, dmaloop)
#   make overlay    copy programs, module and overlay files into ~/mz-overlay
#   make push       copy everything straight to the running board and reload the driver
#   make image      make overlay, then rebuild the Buildroot image
#   make flash      make image, then write it to the SD card (default /dev/sdb, override with SD=/dev/sdc)
#
# Override paths on the command line, for example: make push BOARD=10.0.0.135
#
# The PL bitstream is built by Vivado outside the repo (fpga/*/build.tcl).
# FPGA_BIN is the project 04 design if it is built, else project 02's. It is
# installed on the board as /lib/firmware/pl.bin (loaded by /etc/init.d/S40pl).
# If neither exists, it is skipped.

BR      ?= $(HOME)/br-microzed
OVERLAY ?= $(HOME)/mz-overlay
BOARD   ?= 10.0.0.134
SD      ?= /dev/sdb
FPGA_BIN ?= $(firstword $(wildcard $(HOME)/vivado/mz04_dma/mz04_dma.bin) \
                        $(HOME)/vivado/mz02_axiregs/axiregs.bin)

CC      := $(BR)/host/bin/arm-linux-gcc
CFLAGS  ?= -Wall -Wextra -O2
LDLIBS  := -lgpiod

APPS    := hello blink leddaemon
BINS    := $(addprefix build/,$(APPS))

# Kernel module loaded at boot by /etc/init.d/S30myled
KMOD_DIR := modules/myled-irq
KO       := $(KMOD_DIR)/myled_irq.ko

# PL drivers, loaded at boot by /etc/init.d/S40pl (after the bitstream)
AXI_DIR  := modules/axiregs
AXI_KO   := $(AXI_DIR)/axiregs.ko
DMA_DIR  := modules/dmaloop
DMA_KO   := $(DMA_DIR)/dmaloop.ko

all: $(BINS) module

module:
	$(MAKE) -C $(KMOD_DIR) BR=$(BR)
	$(MAKE) -C $(AXI_DIR) BR=$(BR)
	$(MAKE) -C $(DMA_DIR) BR=$(BR)

build:
	mkdir -p build

build/hello: LDLIBS :=
build/%: apps/%.c | build
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

overlay: all
	cp -a overlay/. $(OVERLAY)/
	install -D -m 755 -t $(OVERLAY)/usr/bin $(BINS)
	install -D -m 644 -t $(OVERLAY)/lib/modules/myled $(KO)
	install -D -m 644 -t $(OVERLAY)/lib/modules/axiregs $(AXI_KO)
	install -D -m 644 -t $(OVERLAY)/lib/modules/dmaloop $(DMA_KO)
	@if [ -f $(FPGA_BIN) ]; then install -D -m 644 $(FPGA_BIN) $(OVERLAY)/lib/firmware/pl.bin; \
	echo "Bitstream: $(FPGA_BIN) -> /lib/firmware/pl.bin"; \
	else echo "WARNING: $(FPGA_BIN) not found, bitstream not copied (run fpga/*/build.tcl)"; fi
	# Retired files: the overlay only adds files, so remove them from the
	# overlay and from Buildroot's target/ (leddaemon -> myled_irq driver,
	# S40axiregs -> S40pl, axiregs.bin -> pl.bin).
	rm -f $(OVERLAY)/etc/init.d/S99leddaemon $(BR)/target/etc/init.d/S99leddaemon
	rm -f $(OVERLAY)/etc/init.d/S40axiregs $(BR)/target/etc/init.d/S40axiregs
	rm -f $(OVERLAY)/lib/firmware/axiregs.bin $(BR)/target/lib/firmware/axiregs.bin
	@echo "Copied into $(OVERLAY). Now run 'make' in $(BR) and re-flash."

push: all
	-ssh root@$(BOARD) '/etc/init.d/S99leddaemon stop 2>/dev/null; rm -f /etc/init.d/S99leddaemon; /etc/init.d/S30myled stop 2>/dev/null; rmmod myled_irq myled_dt myled 2>/dev/null; true'
	-ssh root@$(BOARD) 'for s in S40pl S40axiregs; do [ -x /etc/init.d/$$s ] && /etc/init.d/$$s stop; done; rmmod dmaloop xilinx_dma axiregs 2>/dev/null; rm -f /etc/init.d/S40axiregs /lib/firmware/axiregs.bin; true'
	ssh root@$(BOARD) 'mkdir -p /lib/modules/myled /lib/modules/axiregs /lib/modules/dmaloop /lib/firmware'
	scp -O $(KO) root@$(BOARD):/lib/modules/myled/
	scp -O $(AXI_KO) root@$(BOARD):/lib/modules/axiregs/
	scp -O $(DMA_KO) root@$(BOARD):/lib/modules/dmaloop/
	@if [ -f $(FPGA_BIN) ]; then scp -O $(FPGA_BIN) root@$(BOARD):/lib/firmware/pl.bin; \
	else echo "WARNING: $(FPGA_BIN) not found, bitstream not copied"; fi
	scp -O $(BINS) overlay/usr/bin/* root@$(BOARD):/usr/bin/
	scp -O overlay/srv/www/cgi-bin/* root@$(BOARD):/srv/www/cgi-bin/
	scp -O overlay/etc/init.d/* root@$(BOARD):/etc/init.d/
	ssh root@$(BOARD) 'rm -f /srv/www/index.html; /etc/init.d/S30myled start; /etc/init.d/S40pl start'

image: overlay
	$(MAKE) -C $(BR)

flash: image
	./flash-sd.sh $(SD)

clean:
	rm -rf build
	$(MAKE) -C $(KMOD_DIR) BR=$(BR) clean
	$(MAKE) -C $(AXI_DIR) BR=$(BR) clean
	$(MAKE) -C $(DMA_DIR) BR=$(BR) clean

.PHONY: all module overlay push image flash clean
