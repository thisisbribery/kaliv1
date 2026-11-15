TARGET = KaliSmash
CPP_SOURCES = main.cpp
LIBDAISY_DIR ?= $(HOME)/dev/libDaisy
GCC_PATH ?= $(HOME)/dev/toolchains/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi/bin
OPT = -O2
APP_TYPE ?= BOOT_SRAM
CPPFLAGS += -I../dsp
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
