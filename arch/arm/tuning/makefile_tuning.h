#
# Makefile for the linux kernel.
#

ifeq ($(CONFIG_MACH_STINGRAY),y)
KBUILD_CFLAGS  += $(call cc-option,-O3)
KBUILD_CFLAGS  += $(call cc-option,-ffast-math)
KBUILD_CFLAGS  += $(call cc-option,-fgraphite-identity)
KBUILD_CFLAGS  += $(call cc-option,-floop-block)
KBUILD_CFLAGS  += $(call cc-option,-floop-strip-mine)
KBUILD_CFLAGS  += $(call cc-option,-fmodulo-sched)
KBUILD_CFLAGS  += $(call cc-option,-fsched-spec-load-dangerous)
KBUILD_CFLAGS  += $(call cc-option,-ftree-loop-distribution)
KBUILD_CFLAGS  += $(call cc-option,-mfloat-abi=hard,$(call cc-option,-mhard-float,-msoft-float))
KBUILD_CFLAGS  += $(call cc-option,-mfpu=vfpv3-d16)
KBUILD_CFLAGS  += $(call cc-option,--param l2-cache-size=1024)
KBUILD_CFLAGS  += $(call cc-option,--param l1-cache-size=32)
KBUILD_CFLAGS  += $(call cc-option,--param l1-cache-line-size=128)

#Automatically turned on by -O2
#KBUILD_CFLAGS  += $(call cc-option,-fgcse)
#Automatically turned on by -O3
#KBUILD_CFLAGS  += $(call cc-option,-funswitch-loops)
#KBUILD_CFLAGS  += $(call cc-option,-fpredictive-commoning)
#KBUILD_CFLAGS  += $(call cc-option,-fgcse-after-reload)
#KBUILD_CFLAGS  += $(call cc-option,-ftree-vectorize)
#KBUILD_CFLAGS  += $(call cc-option,-fipa-cp-clone)

KBUILD_AFLAGS  += $(call as-option,-Wa$(comma)-mfloat-abi=hard,$(call as-option,-Wa$(comma)-mhard-float,-Wa$(comma)-msoft-float))
KBUILD_AFLAGS  += $(call as-option,-Wa$(comma)-mfpu=vfpv3-d16)

tune-y         += $(call cc-option,-mtune=cortex-a9)
tune-y         += $(call as-option,-Wa$(comma)-march=armv7-a)
tune-y         += $(call as-option,-Wa$(comma)-mcpu=cortex-a9)
endif # CONFIG_MACH_STINGRAY
