# ################################################################
# Copyright (c) 2019-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# ################################################################

.PHONY: all
all: check_flipped_bits

ZSTDLIBDIR ?= ../../lib

CFLAGS     ?= -O3
CFLAGS     += -I$(ZSTDLIBDIR) -I$(ZSTDLIBDIR)/common -I$(ZSTDLIBDIR)/compress \
              -I$(ZSTDLIBDIR)/decompress
CFLAGS     += -Wall -Wextra -Wcast-qual -Wcast-align -Wshadow                 \
              -Wstrict-aliasing=1 -Wswitch-enum -Wdeclaration-after-statement \
              -Wstrict-prototypes -Wundef                                     \
              -Wvla -Wformat=2 -Winit-self -Wfloat-equal -Wwrite-strings      \
              -Wredundant-decls -Wmissing-prototypes
CFLAGS     += $(DEBUGFLAGS) $(MOREFLAGS)
FLAGS       = $(CPPFLAGS) $(CFLAGS) $(LDFLAGS)

.PHONY: $(ZSTDLIBDIR)/libzstd.a
$(ZSTDLIBDIR)/libzstd.a:
	$(MAKE) -C $(ZSTDLIBDIR) libzstd.a

check_flipped_bits: check_flipped_bits.c $(ZSTDLIBDIR)/libzstd.a
	$(CC) $(FLAGS) $< -o $@$(EXT) $(ZSTDLIBDIR)/libzstd.a

.PHONY: clean
clean:
	rm -f check_flipped_bits
