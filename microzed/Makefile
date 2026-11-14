# MicroZed apps and overlay files.
#
#   make            build the programs (into build/)
#   make overlay    copy programs + overlay files into ~/mz-overlay (then run make in ~/br-microzed)
#   make push       copy programs + scripts straight to the running board and restart services
#   make image      make overlay, then rebuild the Buildroot image
#   make flash      make image, then write it to the SD card (default /dev/sdb, override with SD=/dev/sdc)
#
# Override paths on the command line, for example: make push BOARD=10.0.0.135

BR      ?= $(HOME)/br-microzed
OVERLAY ?= $(HOME)/mz-overlay
BOARD   ?= 10.0.0.134
SD      ?= /dev/sdb

CC      := $(BR)/host/bin/arm-linux-gcc
CFLAGS  ?= -Wall -Wextra -O2
LDLIBS  := -lgpiod

APPS    := hello blink leddaemon
BINS    := $(addprefix build/,$(APPS))

all: $(BINS)

build:
	mkdir -p build

build/hello: LDLIBS :=
build/%: apps/%.c | build
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

overlay: all
	cp -a overlay/. $(OVERLAY)/
	install -D -m 755 -t $(OVERLAY)/usr/bin $(BINS)
	@echo "Copied into $(OVERLAY). Now run 'make' in $(BR) and re-flash."

push: all
	-ssh root@$(BOARD) '/etc/init.d/S99leddaemon stop; kill $$(cat /tmp/led.pid) 2>/dev/null; rm -f /tmp/led.pid'
	scp -O $(BINS) root@$(BOARD):/usr/bin/
	scp -O overlay/srv/www/cgi-bin/* root@$(BOARD):/srv/www/cgi-bin/
	scp -O overlay/etc/init.d/S90httpd overlay/etc/init.d/S99leddaemon root@$(BOARD):/etc/init.d/
	ssh root@$(BOARD) 'rm -f /srv/www/index.html; /etc/init.d/S99leddaemon start'

image: overlay
	$(MAKE) -C $(BR)

flash: image
	./flash-sd.sh $(SD)

clean:
	rm -rf build

.PHONY: all overlay push image flash clean
