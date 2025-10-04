# ################################################################
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# ################################################################

# This Makefile presumes libzstd is built, using `make` in / or /lib/

ZSTDLIB_PATH = ../../../lib
ZSTDLIB_NAME = libzstd.a
ZSTDLIB = $(ZSTDLIB_PATH)/$(ZSTDLIB_NAME)

CPPFLAGS += -DXXH_NAMESPACE=ZSTD_ -I../ -I$(ZSTDLIB_PATH) -I$(ZSTDLIB_PATH)/common

CFLAGS ?= -O3
CFLAGS += -g -Wall -Wextra -Wcast-qual -Wcast-align -Wconversion \
          -Wformat=2 -Wstrict-aliasing=1

SEEKABLE_OBJS = ../zstdseek_compress.c ../zstdseek_decompress.c $(ZSTDLIB)

.PHONY: default clean test
default: test

test: seekable_tests
	./seekable_tests

$(ZSTDLIB):
	$(MAKE) -C $(ZSTDLIB_PATH) $(ZSTDLIB_NAME)

seekable_tests : $(SEEKABLE_OBJS)

clean:
	@$(RM) core *.o tmp* result* *.zst \
		seekable_tests
	@echo Cleaning completed
