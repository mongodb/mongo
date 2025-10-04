# ################################################################
# Copyright (c) Yann Collet, Meta Platforms, Inc.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# ################################################################

PROGDIR = ../../programs
LIBDIR  = ../../lib

LIBZSTD = $(LIBDIR)/libzstd.a

CPPFLAGS+= -I$(LIBDIR) -I$(LIBDIR)/compress -I$(LIBDIR)/common

CFLAGS  ?= -O3
CFLAGS  += -std=gnu99
DEBUGFLAGS= -Wall -Wextra -Wcast-qual -Wcast-align -Wshadow \
            -Wstrict-aliasing=1 -Wswitch-enum \
            -Wstrict-prototypes -Wundef -Wpointer-arith \
            -Wvla -Wformat=2 -Winit-self -Wfloat-equal -Wwrite-strings \
            -Wredundant-decls
CFLAGS  += $(DEBUGFLAGS) $(MOREFLAGS)

default: externalSequenceProducer

all: externalSequenceProducer

externalSequenceProducer: sequence_producer.c main.c $(LIBZSTD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDFLAGS) -o $@

.PHONY: $(LIBZSTD)
$(LIBZSTD):
	$(MAKE) -C $(LIBDIR) libzstd.a CFLAGS="$(CFLAGS)"

clean:
	$(RM) *.o
	$(MAKE) -C $(LIBDIR) clean > /dev/null
	$(RM) externalSequenceProducer
