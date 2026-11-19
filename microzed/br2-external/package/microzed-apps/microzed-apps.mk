################################################################################
#
# microzed-apps
#
# Our user-space programs (microzed/apps/), one .c file each. A generic
# package: we write the build and install commands ourselves.
#
################################################################################

MICROZED_APPS_VERSION = 1.0
# Run "make microzed-apps-rebuild" after editing the source.
MICROZED_APPS_SITE = $(BR2_EXTERNAL_MICROZED_PATH)/../apps
MICROZED_APPS_SITE_METHOD = local

# Built before us: libgpiod (blink links with it) and microzed-modules
# (installs adcstream.h in staging).
MICROZED_APPS_DEPENDENCIES = libgpiod microzed-modules

MICROZED_APPS_PROGRAMS = hello blink adcread

# TARGET_CC is the cross-compiler; it finds headers and libraries in the
# staging tree by itself. -O2 last, as in the old hand-made builds (adcread's
# speed was measured with it).
MICROZED_APPS_CFLAGS = $(TARGET_CFLAGS) -Wall -Wextra -O2

define MICROZED_APPS_BUILD_CMDS
	$(TARGET_CC) $(MICROZED_APPS_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/hello $(@D)/hello.c
	$(TARGET_CC) $(MICROZED_APPS_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/blink $(@D)/blink.c -lgpiod
	$(TARGET_CC) $(MICROZED_APPS_CFLAGS) \
		-I$(STAGING_DIR)/usr/include/microzed $(TARGET_LDFLAGS) \
		-o $(@D)/adcread $(@D)/adcread.c
endef

define MICROZED_APPS_INSTALL_TARGET_CMDS
	$(foreach p,$(MICROZED_APPS_PROGRAMS),\
		$(INSTALL) -D -m 0755 $(@D)/$(p) $(TARGET_DIR)/usr/bin/$(p)$(sep))
endef

$(eval $(generic-package))
