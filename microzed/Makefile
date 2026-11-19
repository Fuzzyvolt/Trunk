# MicroZed apps and overlay files.
#
#   make            build the programs (into build/) and the kernel modules (myled_irq, axiregs, pltimer, dmaloop, adcstream)
#   make overlay    copy programs, module and overlay files into ~/mz-overlay
#   make push       copy everything straight to the running board and reload the driver
#   make image      make overlay, then rebuild the Buildroot image
#   make flash      make image, then write it to the SD card (default /dev/sdb, override with SD=/dev/sdc)
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

CC      := $(BR)/host/bin/arm-linux-gcc
CFLAGS  ?= -Wall -Wextra -O2
LDLIBS  := -lgpiod

APPS    := hello blink leddaemon adcread
BINS    := $(addprefix build/,$(APPS))

# Kernel module loaded at boot by /etc/init.d/S30myled
KMOD_DIR := modules/myled-irq
KO       := $(KMOD_DIR)/myled_irq.ko

# PL drivers, loaded at boot by /etc/init.d/S40pl (after the bitstream)
AXI_DIR  := modules/axiregs
AXI_KO   := $(AXI_DIR)/axiregs.ko
PLT_DIR  := modules/pltimer
PLT_KO   := $(PLT_DIR)/pltimer.ko
DMA_DIR  := modules/dmaloop
DMA_KO   := $(DMA_DIR)/dmaloop.ko
ADC_DIR  := modules/adcstream
ADC_KO   := $(ADC_DIR)/adcstream.ko

all: $(BINS) module

module:
	$(MAKE) -C $(KMOD_DIR) BR=$(BR)
	$(MAKE) -C $(AXI_DIR) BR=$(BR)
	$(MAKE) -C $(PLT_DIR) BR=$(BR)
	$(MAKE) -C $(DMA_DIR) BR=$(BR)
	$(MAKE) -C $(ADC_DIR) BR=$(BR)

build:
	mkdir -p build

build/hello build/adcread: LDLIBS :=
# adcread includes the driver's ioctl/mmap header
build/adcread: CFLAGS += -Imodules/adcstream
build/adcread: modules/adcstream/adcstream.h
build/%: apps/%.c | build
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

overlay: all
	cp -a overlay/. $(OVERLAY)/
	install -D -m 755 -t $(OVERLAY)/usr/bin $(BINS)
	install -D -m 644 -t $(OVERLAY)/lib/modules/myled $(KO)
	install -D -m 644 -t $(OVERLAY)/lib/modules/axiregs $(AXI_KO)
	install -D -m 644 -t $(OVERLAY)/lib/modules/pltimer $(PLT_KO)
	install -D -m 644 -t $(OVERLAY)/lib/modules/dmaloop $(DMA_KO)
	install -D -m 644 -t $(OVERLAY)/lib/modules/adcstream $(ADC_KO)
	@if [ -f $(FPGA_BIN) ]; then install -D -m 644 $(FPGA_BIN) $(OVERLAY)/lib/firmware/pl.bin; \
	echo "Bitstream: $(FPGA_BIN) -> /lib/firmware/pl.bin"; \
	else echo "WARNING: $(FPGA_BIN) not found, bitstream not copied (run fpga/05-adc-stream/build.tcl)"; fi
	# Retired files (leddaemon: replaced by the driver; S40axiregs: replaced by
	# S40pl; per-project bitstream names: replaced by pl.bin). The overlay only
	# adds files, so remove them from the overlay and from Buildroot's target/.
	rm -f $(OVERLAY)/etc/init.d/S99leddaemon $(BR)/target/etc/init.d/S99leddaemon
	rm -f $(OVERLAY)/etc/init.d/S40axiregs $(BR)/target/etc/init.d/S40axiregs
	rm -f $(OVERLAY)/lib/firmware/axiregs.bin $(BR)/target/lib/firmware/axiregs.bin
	rm -f $(OVERLAY)/lib/firmware/mz03_pltimer.bin $(BR)/target/lib/firmware/mz03_pltimer.bin
	@echo "Copied into $(OVERLAY). Now run 'make' in $(BR) and re-flash."

push: all
	-ssh root@$(BOARD) '/etc/init.d/S99leddaemon stop 2>/dev/null; rm -f /etc/init.d/S99leddaemon; /etc/init.d/S30myled stop 2>/dev/null; rmmod myled_irq myled_dt myled 2>/dev/null; true'
	-ssh root@$(BOARD) 'for s in S40axiregs S40pl; do [ -x /etc/init.d/$$s ] && /etc/init.d/$$s stop; done; rm -f /etc/init.d/S40axiregs /lib/firmware/axiregs.bin /lib/firmware/mz03_pltimer.bin; rmmod adcstream dmaloop xilinx_dma pltimer axiregs 2>/dev/null; true'
	ssh root@$(BOARD) 'mkdir -p /lib/modules/myled /lib/modules/axiregs /lib/modules/pltimer /lib/modules/dmaloop /lib/modules/adcstream /lib/firmware'
	scp -O $(KO) root@$(BOARD):/lib/modules/myled/
	scp -O $(AXI_KO) root@$(BOARD):/lib/modules/axiregs/
	scp -O $(PLT_KO) root@$(BOARD):/lib/modules/pltimer/
	scp -O $(DMA_KO) root@$(BOARD):/lib/modules/dmaloop/
	scp -O $(ADC_KO) root@$(BOARD):/lib/modules/adcstream/
	@if [ -f $(FPGA_BIN) ]; then scp -O $(FPGA_BIN) root@$(BOARD):/lib/firmware/pl.bin; \
	else echo "WARNING: $(FPGA_BIN) not found, bitstream not copied"; fi
	scp -O $(BINS) overlay/usr/bin/* root@$(BOARD):/usr/bin/
	scp -O overlay/srv/www/cgi-bin/* root@$(BOARD):/srv/www/cgi-bin/
	scp -O overlay/srv/www/scope.html root@$(BOARD):/srv/www/
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
	$(MAKE) -C $(PLT_DIR) BR=$(BR) clean
	$(MAKE) -C $(DMA_DIR) BR=$(BR) clean
	$(MAKE) -C $(ADC_DIR) BR=$(BR) clean

.PHONY: all module overlay push image flash clean
