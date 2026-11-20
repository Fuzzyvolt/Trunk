################################################################################
#
# microzed-modules
#
# Our out-of-tree kernel modules (microzed/modules/), built with Buildroot's
# kernel-module infrastructure. For each folder in MODULE_SUBDIRS it runs
#   make -C <kernel tree> M=<folder> modules
#   make -C <kernel tree> M=<folder> modules_install
# with the same compiler and flags as the kernel itself. Buildroot then runs
# depmod on the target (after all packages, before the rootfs overlay), so
# modprobe finds the modules.
#
################################################################################

MICROZED_MODULES_VERSION = 1.0
# The source is in this repo: "local" copies (rsync) it into
# build/microzed-modules-1.0/ instead of downloading it. Buildroot does not
# notice source changes: run "make microzed-modules-rebuild" after editing.
MICROZED_MODULES_SITE = $(BR2_EXTERNAL_MICROZED_PATH)/../modules
MICROZED_MODULES_SITE_METHOD = local
MICROZED_MODULES_LICENSE = GPL-2.0

# Leftovers from building by hand in microzed/modules/ are not copied.
MICROZED_MODULES_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = \
	--exclude '*.o' --exclude '*.ko' --exclude '*.mod' --exclude '*.mod.c' \
	--exclude '.*.cmd' --exclude modules.order --exclude Module.symvers

# Folders in microzed/modules/ to build (hello is a template for quick
# experiments, not shipped).
MICROZED_MODULES_MODULE_SUBDIRS = myled-irq axiregs pltimer dmaloop adcstream adciio

# Install in /lib/modules/<version>/microzed/ (the kernel's default for
# out-of-tree modules is updates/; our own folder name is easier to find).
MICROZED_MODULES_MODULE_MAKE_OPTS = INSTALL_MOD_DIR=microzed

# adcstream.h is shared with user space (apps/adcread.c): put it in the
# staging tree (the cross-compiler's sysroot), like a library's header.
MICROZED_MODULES_INSTALL_STAGING = YES
define MICROZED_MODULES_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/adcstream/adcstream.h \
		$(STAGING_DIR)/usr/include/microzed/adcstream.h
endef

# kernel-module adds the build and install steps (as hooks) and the
# dependency on the linux package; generic-package makes it a package.
$(eval $(kernel-module))
$(eval $(generic-package))
