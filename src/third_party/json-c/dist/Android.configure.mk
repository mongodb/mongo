# This file is the top android makefile for all sub-modules.
#
# Suggested settings to build for Android:
#
# export PATH=$PATH:/opt/android-ndk/toolchains/arm-linux-androideabi-4.9/prebuilt/linux-x86_64/bin/
# export SYSROOT=/opt/android-ndk/platforms/android-9/arch-arm/usr/
# export LD=arm-linux-androideabi-ld
# export CC="arm-linux-androideabi-gcc --sysroot=/opt/android-ndk/platforms/android-9/arch-arm"
#
# Then run autogen.sh, configure and make.
#

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

json_c_TOP := $(LOCAL_PATH)

JSON_C_BUILT_SOURCES := Android.mk

JSON_C_BUILT_SOURCES := $(patsubst %, $(abspath $(json_c_TOP))/%, $(JSON_C_BUILT_SOURCES))

.PHONY: json-c-configure json-c-configure-real
json-c-configure-real:
	echo $(JSON_C_BUILT_SOURCES)
	cd $(json_c_TOP) ; \
	$(abspath $(json_c_TOP))/autogen.sh && \
	CC="$(CONFIGURE_CC)" \
	CFLAGS="$(CONFIGURE_CFLAGS)" \
	LD=$(TARGET_LD) \
	LDFLAGS="$(CONFIGURE_LDFLAGS)" \
	CPP=$(CONFIGURE_CPP) \
	CPPFLAGS="$(CONFIGURE_CPPFLAGS)" \
	PKG_CONFIG_LIBDIR=$(CONFIGURE_PKG_CONFIG_LIBDIR) \
	PKG_CONFIG_TOP_BUILD_DIR=/ \
	ac_cv_func_malloc_0_nonnull=yes \
	ac_cv_func_realloc_0_nonnull=yes \
	$(abspath $(json_c_TOP))/$(CONFIGURE) --host=$(CONFIGURE_HOST) \
	--prefix=/system \
	&& \
	for file in $(JSON_C_BUILT_SOURCES); do \
		rm -f $$file && \
		make -C $$(dirname $$file) $$(basename $$file) ; \
	done

json-c-configure: json-c-configure-real

PA_CONFIGURE_TARGETS += json-c-configure

-include $(json_c_TOP)/Android.mk
