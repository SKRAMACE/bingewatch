ifeq ($(BUILD_CONFIG),arm_cortex-a9.gcc4.8_uclibc_openwrt)
    TOOL_DIR=/opt/toolchains/arm_cortex-a9.gcc4.8_uclibc_openwrt/bin
    CROSS_PREFIX=arm-openwrt-linux-
    CFLAGS+=-mfpu=neon -mfloat-abi=hard -mno-unaligned-access -ggdb -O3
    export STAGING_DIR=$(TOOL_DIR)
    CONFIG_FLAGS="--host=arm-openwrt-linux"
    ARCH=arm_cortex-a9
endif

ifeq ($(BUILD_CONFIG),x86_32.gcc)
    TOOL_DIR=/usr/bin
    CROSS_PREFIX=
    CFLAGS+=-m32
    ARCH=x86_32
endif

ifeq ($(BUILD_CONFIG),x86_64.gcc)
    TOOL_DIR=/usr/bin
    CROSS_PREFIX=
    CFLAGS+= -g -gdwarf-3
    ARCH=x86_64
endif

ifeq ($(BUILD_CONFIG),x86_64.gcc.libc2.12)
    TOOL_DIR=/usr/bin
    CROSS_PREFIX=
    CFLAGS+= -g -gdwarf-3 -O3
    ARCH=x86_64
endif

ifeq ($(BUILD_CONFIG),arm_cortex-a9.gcc4.8_gnueabi_timesys)
    TOOL_DIR=/opt/toolchains/arm_cortex-a9.gcc4.8_gnueabi_timesys/bin
    CROSS_PREFIX=armv7l-timesys-linux-gnueabi-
    CFLAGS+=-g -Wall -fPIC -mfpu=neon -mfloat-abi=softfp
    ARCH=arm_cortex-a9__timesys
endif

ifeq ($(BUILD_CONFIG),arm_cortex-a9.gcc4.8_gnueabihf_linux)
    TOOL_DIR=/opt/toolchains/arm_cortex-a9.gcc4.8_gnueabihf_linux/bin
    CROSS_PREFIX=arm-linux-gnueabihf-
    CFLAGS+=-g -Wall -fPIC -O3 -fomit-frame-pointer -fstrict-aliasing -ffast-math -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
    ARCH=arm_cortex-a9.jhf
endif

##############################################
#
# Define tool chain based on TOOL_DIR and CROSS_PREFIX
#
##############################################

CC     := $(TOOL_DIR)/$(CROSS_PREFIX)gcc
CXX    := $(TOOL_DIR)/$(CROSS_PREFIX)g++
LD     := $(TOOL_DIR)/$(CROSS_PREFIX)ld
NM     := $(TOOL_DIR)/$(CROSS_PREFIX)nm
OBJCOPY:= $(TOOL_DIR)/$(CROSS_PREFIX)objcopy
AR     := $(TOOL_DIR)/$(CROSS_PREFIX)ar
RANLIB := $(TOOL_DIR)/$(CROSS_PREFIX)ranlib
STRIP  := $(TOOL_DIR)/$(CROSS_PREFIX)strip
