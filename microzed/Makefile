# MicroZed apps and overlay files.
#
#   make            build the programs (into build/) and the kernel modules (myled_irq, axiregs)
#   make overlay    copy programs, module and overlay files into ~/mz-overlay
#   make push       copy everything straight to the running board and reload the driver
#   make image      make overlay, then rebuild the Buildroot image
#   make flash      make image, then write it to the SD card (default /dev/sdb, override with SD=/dev/sdc)
#
# Override paths on the command line, for example: make push BOARD=10.0.0.135
#
# The PL bitstream (project 02) is built by Vivado outside the repo
# (fpga/02-axiregs/build.tcl). If FPGA_BIN does not exist yet, it is skipped.

BR      ?= $(HOME)/br-microzed
OVERLAY ?= $(HOME)/mz-overlay
BOARD   ?= 10.0.0.134
SD      ?= /dev/sdb
FPGA_BIN ?= $(HOME)/vivado/mz02_axiregs/axiregs.bin

CC      := $(BR)/host/bin/arm-linux-gcc
CFLAGS  ?= -Wall -Wextra -O2
LDLIBS  := -lgpiod

APPS    := hello blink leddaemon
BINS    := $(addprefix build/,$(APPS))

# Kernel module loaded at boot by /etc/init.d/S30myled
KMOD_DIR := modules/myled-irq
KO       := $(KMOD_DIR)/myled_irq.ko

# Project 02 driver, loaded at boot by /etc/init.d/S40axiregs (after the bitstream)
AXI_DIR  := modules/axiregs
AXI_KO   := $(AXI_DIR)/axiregs.ko

all: $(BINS) module

module:
	$(MAKE) -C $(KMOD_DIR) BR=$(BR)
	$(MAKE) -C $(AXI_DIR) BR=$(BR)

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
	@if [ -f $(FPGA_BIN) ]; then install -D -m 644 -t $(OVERLAY)/lib/firmware $(FPGA_BIN); \
	else echo "WARNING: $(FPGA_BIN) not found, bitstream not copied (run fpga/02-axiregs/build.tcl)"; fi
	# leddaemon is retired (the driver does its job). The overlay only adds files,
	# so remove its old init script from the overlay and from Buildroot's target/.
	rm -f $(OVERLAY)/etc/init.d/S99leddaemon $(BR)/target/etc/init.d/S99leddaemon
	@echo "Copied into $(OVERLAY). Now run 'make' in $(BR) and re-flash."

push: all
	-ssh root@$(BOARD) '/etc/init.d/S99leddaemon stop 2>/dev/null; rm -f /etc/init.d/S99leddaemon; /etc/init.d/S30myled stop 2>/dev/null; rmmod myled_irq myled_dt myled 2>/dev/null; true'
	-ssh root@$(BOARD) '/etc/init.d/S40axiregs stop 2>/dev/null; rmmod axiregs 2>/dev/null; true'
	ssh root@$(BOARD) 'mkdir -p /lib/modules/myled /lib/modules/axiregs /lib/firmware'
	scp -O $(KO) root@$(BOARD):/lib/modules/myled/
	scp -O $(AXI_KO) root@$(BOARD):/lib/modules/axiregs/
	@if [ -f $(FPGA_BIN) ]; then scp -O $(FPGA_BIN) root@$(BOARD):/lib/firmware/; \
	else echo "WARNING: $(FPGA_BIN) not found, bitstream not copied"; fi
	scp -O $(BINS) root@$(BOARD):/usr/bin/
	scp -O overlay/srv/www/cgi-bin/* root@$(BOARD):/srv/www/cgi-bin/
	scp -O overlay/etc/init.d/* root@$(BOARD):/etc/init.d/
	ssh root@$(BOARD) 'rm -f /srv/www/index.html; /etc/init.d/S30myled start; /etc/init.d/S40axiregs start'

image: overlay
	$(MAKE) -C $(BR)

flash: image
	./flash-sd.sh $(SD)

clean:
	rm -rf build
	$(MAKE) -C $(KMOD_DIR) BR=$(BR) clean
	$(MAKE) -C $(AXI_DIR) BR=$(BR) clean

.PHONY: all module overlay push image flash clean
