# Out-of-tree kernel module, built against the Buildroot kernel.
#
#   make        build myled_dt.ko
#   make push   build, then copy myled_dt.ko to the board (/root/)
#   make clean
#
# The kernel build system (Kbuild) reads this file twice: once from here
# (KERNELRELEASE empty), and again from inside the kernel tree, where only
# the obj-m line matters.

ifneq ($(KERNELRELEASE),)

obj-m := myled_dt.o

else

BR    ?= $(HOME)/br-microzed
# The kernel source tree Buildroot built (build/linux-<version> or build/linux-custom)
KDIR  ?= $(shell ls -d $(BR)/build/linux-* 2>/dev/null | grep -v -e headers -e firmware | head -n1)
CROSS ?= $(BR)/host/bin/arm-linux-
BOARD ?= 10.0.0.134

KMAKE  = $(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm CROSS_COMPILE=$(CROSS)

all:
	@test -n "$(KDIR)" || { echo "Kernel tree not found in $(BR)/build/"; exit 1; }
	$(KMAKE) modules

push: all
	scp -O myled_dt.ko root@$(BOARD):/root/

clean:
	$(KMAKE) clean

.PHONY: all push clean

endif
