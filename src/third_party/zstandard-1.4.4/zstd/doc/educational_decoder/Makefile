# ################################################################
# Copyright (c) 2016-present, Yann Collet, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# ################################################################

ZSTD ?= zstd   # note: requires zstd installation on local system

UNAME?= $(shell uname)
ifeq ($(UNAME), SunOS)
DIFF ?= gdiff
else
DIFF ?= diff
endif

HARNESS_FILES=*.c

MULTITHREAD_LDFLAGS = -pthread
DEBUGFLAGS= -g -DZSTD_DEBUG=1
CPPFLAGS += -I$(ZSTDDIR) -I$(ZSTDDIR)/common -I$(ZSTDDIR)/compress \
            -I$(ZSTDDIR)/dictBuilder -I$(ZSTDDIR)/deprecated -I$(PRGDIR)
CFLAGS   ?= -O2
CFLAGS   += -Wall -Wextra -Wcast-qual -Wcast-align -Wshadow                 \
            -Wstrict-aliasing=1 -Wswitch-enum                               \
            -Wredundant-decls -Wstrict-prototypes -Wundef                   \
            -Wvla -Wformat=2 -Winit-self -Wfloat-equal -Wwrite-strings      \
            -std=c99
CFLAGS   += $(DEBUGFLAGS)
CFLAGS   += $(MOREFLAGS)
FLAGS     = $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(MULTITHREAD_LDFLAGS)

harness: $(HARNESS_FILES)
	$(CC) $(FLAGS) $^ -o $@

clean:
	@$(RM) harness
	@$(RM) -rf harness.dSYM  # MacOS specific

test: harness
	#
	# Testing single-file decompression with educational decoder
	#
	@$(ZSTD) -f README.md -o tmp.zst
	@./harness tmp.zst tmp
	@$(DIFF) -s tmp README.md
	@$(RM) tmp*
	#
	# Testing dictionary decompression with education decoder
	#
	# note : files are presented multiple for training, to reach minimum threshold
	@$(ZSTD) --train harness.c zstd_decompress.c zstd_decompress.h README.md \
                  harness.c zstd_decompress.c zstd_decompress.h README.md \
                  harness.c zstd_decompress.c zstd_decompress.h README.md \
                  -o dictionary
	@$(ZSTD) -f README.md -D dictionary -o tmp.zst
	@./harness tmp.zst tmp dictionary
	@$(DIFF) -s tmp README.md
	@$(RM) tmp* dictionary
	@$(MAKE) clean
