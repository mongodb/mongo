# Copyright (c) Yann Collet, Meta Platforms, Inc.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# You may select, at your option, one of the above-listed licenses.

CC = clang
CFLAGS = -g -fno-omit-frame-pointer -fsanitize=undefined,address,fuzzer -I../ -I../../../lib/

.PHONY: default
default: example_seq_prod.o

example_seq_prod.o: example_seq_prod.c
	$(CC) -c $(CFLAGS) $^ -o $@
