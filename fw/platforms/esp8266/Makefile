APP ?= mongoose-os
APP_PLATFORM = esp8266
ifeq "$(APP)" "mongoose-os"
MGOS_PATH = ../../..
endif

# This defines "all" and "clean" targets.
include $(MGOS_PATH)/fw/docker_common.mk

GDB = xtensa-lx106-elf-gdb
EXTRA_GDB_ARGS = -ex 'add-symbol-file $(DOCKER_MGOS_PATH)/common/platforms/$(APP_PLATFORM)/rom/rom.elf 0x40000000'
EXTRA_SERVE_CORE_ARGS = --rom=$(DOCKER_MGOS_PATH)/common/platforms/esp8266/rom/rom.bin --rom_addr=0x40000000
include $(MGOS_PATH)/common/scripts/debug_coredump.mk
