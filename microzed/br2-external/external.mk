# Read every package's .mk file.
include $(sort $(wildcard $(BR2_EXTERNAL_MICROZED_PATH)/package/*/*.mk))
