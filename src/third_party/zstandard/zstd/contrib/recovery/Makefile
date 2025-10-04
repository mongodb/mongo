# ################################################################
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# ################################################################

.PHONY: all
all: recover_directory

ZSTDLIBDIR ?= ../../lib
PROGRAMDIR ?= ../../programs

CFLAGS     ?= -O3
CFLAGS     += -I$(ZSTDLIBDIR) -I$(PROGRAMDIR)
CFLAGS     += -Wall -Wextra -Wcast-qual -Wcast-align -Wshadow                 \
              -Wstrict-aliasing=1 -Wswitch-enum                               \
              -Wstrict-prototypes -Wundef                                     \
              -Wvla -Wformat=2 -Winit-self -Wfloat-equal -Wwrite-strings      \
              -Wredundant-decls -Wmissing-prototypes
CFLAGS     += $(DEBUGFLAGS) $(MOREFLAGS)
FLAGS       = $(CPPFLAGS) $(CFLAGS) $(LDFLAGS)

.PHONY: $(ZSTDLIBDIR)/libzstd.a
$(ZSTDLIBDIR)/libzstd.a:
	$(MAKE) -C $(ZSTDLIBDIR) libzstd.a

recover_directory: recover_directory.c $(ZSTDLIBDIR)/libzstd.a $(PROGRAMDIR)/util.c
	$(CC) $(FLAGS) $^ -o $@$(EXT)

.PHONY: clean
clean:
	rm -f recover_directory
