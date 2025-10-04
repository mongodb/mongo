# ################################################################
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# You may select, at your option, one of the above-listed licenses.
# ################################################################

.PHONY: libzstd
libzstd:
	rm -rf linux
	mkdir -p linux
	mkdir -p linux/include/linux
	mkdir -p linux/lib/zstd
	../freestanding_lib/freestanding.py \
		--source-lib ../../lib \
		--output-lib linux/lib/zstd \
		--xxhash '<linux/xxhash.h>' \
		--xxh64-state 'struct xxh64_state' \
		--xxh64-prefix 'xxh64' \
		--rewrite-include '<limits\.h>=<linux/limits.h>' \
		--rewrite-include '<stddef\.h>=<linux/types.h>' \
		--rewrite-include '"\.\./zstd.h"=<linux/zstd.h>' \
		--rewrite-include '"(\.\./)?zstd_errors.h"=<linux/zstd_errors.h>' \
		--sed 's,/\*\*\*,/* *,g' \
		--sed 's,/\*\*,/*,g' \
		--spdx \
		-DZSTD_NO_INTRINSICS \
		-DZSTD_NO_UNUSED_FUNCTIONS \
		-DZSTD_LEGACY_SUPPORT=0 \
		-DZSTD_STATIC_LINKING_ONLY \
		-DFSE_STATIC_LINKING_ONLY \
		-DXXH_STATIC_LINKING_ONLY \
		-D__GNUC__ \
		-D__linux__=1 \
		-DSTATIC_BMI2=0 \
		-DZSTD_ADDRESS_SANITIZER=0 \
		-DZSTD_MEMORY_SANITIZER=0 \
		-DZSTD_DATAFLOW_SANITIZER=0 \
		-DZSTD_COMPRESS_HEAPMODE=1 \
		-UNO_PREFETCH \
		-U__cplusplus \
		-UZSTD_DLL_EXPORT \
		-UZSTD_DLL_IMPORT \
		-U__ICCARM__ \
		-UZSTD_MULTITHREAD \
		-U_MSC_VER \
		-U_WIN32 \
		-RZSTDLIB_VISIBLE= \
		-RZSTDERRORLIB_VISIBLE= \
		-RZSTD_FALLTHROUGH=fallthrough \
		-DZSTD_HAVE_WEAK_SYMBOLS=0 \
		-DZSTD_TRACE=0 \
		-DZSTD_NO_TRACE \
		-DZSTD_DISABLE_ASM \
		-DZSTD_LINUX_KERNEL
	rm linux/lib/zstd/decompress/huf_decompress_amd64.S
	mv linux/lib/zstd/zstd.h linux/include/linux/zstd_lib.h
	mv linux/lib/zstd/zstd_errors.h linux/include/linux/
	cp linux_zstd.h linux/include/linux/zstd.h
	cp zstd_common_module.c linux/lib/zstd
	cp zstd_compress_module.c linux/lib/zstd
	cp zstd_decompress_module.c linux/lib/zstd
	cp decompress_sources.h linux/lib/zstd
	cp linux.mk linux/lib/zstd/Makefile

LINUX ?= $(HOME)/repos/linux

.PHONY: import
import: libzstd
	rm -f $(LINUX)/include/linux/zstd.h
	rm -f $(LINUX)/include/linux/zstd_errors.h
	rm -rf $(LINUX)/lib/zstd
	cp linux/include/linux/zstd.h $(LINUX)/include/linux
	cp linux/include/linux/zstd_lib.h $(LINUX)/include/linux
	cp linux/include/linux/zstd_errors.h $(LINUX)/include/linux
	cp -r linux/lib/zstd $(LINUX)/lib

import-upstream:
	rm -rf $(LINUX)/lib/zstd
	mkdir $(LINUX)/lib/zstd
	cp ../../lib/zstd.h $(LINUX)/include/linux/zstd_lib.h
	cp -r ../../lib/common $(LINUX)/lib/zstd
	cp -r ../../lib/compress $(LINUX)/lib/zstd
	cp -r ../../lib/decompress $(LINUX)/lib/zstd
	mv $(LINUX)/lib/zstd/zstd_errors.h $(LINUX)/include/linux
	rm $(LINUX)/lib/zstd/common/threading.*
	rm $(LINUX)/lib/zstd/common/pool.*
	rm $(LINUX)/lib/zstd/common/xxhash.*
	rm $(LINUX)/lib/zstd/compress/zstdmt_*

DEBUGFLAGS= -Wall -Wextra -Wcast-qual -Wcast-align -Wshadow \
            -Wstrict-aliasing=1 -Wswitch-enum -Wdeclaration-after-statement \
            -Wstrict-prototypes -Wundef -Wpointer-arith \
            -Wvla -Wformat=2 -Winit-self -Wfloat-equal -Wwrite-strings \
            -Wredundant-decls -Wmissing-prototypes -Wc++-compat \
	    -Wimplicit-fallthrough

.PHONY: test
test: libzstd
	$(MAKE) -C test run-test CFLAGS="-O3 $(CFLAGS) $(DEBUGFLAGS) -Werror" -j

.PHONY: clean
clean:
	$(RM) -rf linux
	$(MAKE) -C test clean
