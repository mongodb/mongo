# ################################################################
# Copyright (c) 2016-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# ################################################################

# Optionally user defined flags
CFLAGS ?= -O3
CXXFLAGS ?= -O3
CPPFLAGS ?=
LDFLAGS ?=
ARFLAGS ?=
LIB_FUZZING_ENGINE ?= libregression.a
PYTHON ?= python
ifeq ($(shell uname), Darwin)
	DOWNLOAD?=curl -L -o
else
	DOWNLOAD?=wget -O
endif
CORPORA_URL_PREFIX:=https://github.com/facebook/zstd/releases/download/fuzz-corpora/

ZSTDDIR = ../../lib
PRGDIR = ../../programs

FUZZ_CPPFLAGS := -I$(ZSTDDIR) -I$(ZSTDDIR)/common -I$(ZSTDDIR)/compress \
	-I$(ZSTDDIR)/dictBuilder -I$(ZSTDDIR)/deprecated -I$(ZSTDDIR)/legacy \
	-I$(PRGDIR) -DZSTD_MULTITHREAD -DZSTD_LEGACY_SUPPORT=1 $(CPPFLAGS)
FUZZ_EXTRA_FLAGS := -Wall -Wextra -Wcast-qual -Wcast-align -Wshadow \
	-Wstrict-aliasing=1 -Wswitch-enum -Wdeclaration-after-statement \
	-Wstrict-prototypes -Wundef \
	-Wvla -Wformat=2 -Winit-self -Wfloat-equal -Wwrite-strings \
	-Wredundant-decls \
	-g -fno-omit-frame-pointer
FUZZ_CFLAGS := $(FUZZ_EXTRA_FLAGS) $(CFLAGS)
FUZZ_CXXFLAGS := $(FUZZ_EXTRA_FLAGS) -std=c++11 $(CXXFLAGS)
FUZZ_LDFLAGS := -pthread $(LDFLAGS)
FUZZ_ARFLAGS := $(ARFLAGS)
FUZZ_TARGET_FLAGS = $(FUZZ_CPPFLAGS) $(FUZZ_CXXFLAGS) $(FUZZ_LDFLAGS)

FUZZ_HEADERS := fuzz_helpers.h fuzz.h zstd_helpers.h fuzz_data_producer.h
FUZZ_SRC := $(PRGDIR)/util.c zstd_helpers.c fuzz_data_producer.c

ZSTDCOMMON_SRC := $(ZSTDDIR)/common/*.c
ZSTDCOMP_SRC   := $(ZSTDDIR)/compress/*.c
ZSTDDECOMP_SRC := $(ZSTDDIR)/decompress/*.c
ZSTDDICT_SRC := $(ZSTDDIR)/dictBuilder/*.c
ZSTDLEGACY_SRC := $(ZSTDDIR)/legacy/*.c
FUZZ_SRC       := \
	$(FUZZ_SRC) \
	$(ZSTDDECOMP_SRC) \
	$(ZSTDCOMMON_SRC) \
	$(ZSTDCOMP_SRC) \
	$(ZSTDDICT_SRC) \
	$(ZSTDLEGACY_SRC)

FUZZ_OBJ := $(patsubst %.c,%.o, $(wildcard $(FUZZ_SRC)))


.PHONY: default all clean cleanall

default: all

FUZZ_TARGETS :=       \
	simple_round_trip \
	stream_round_trip \
	block_round_trip  \
	simple_decompress \
	stream_decompress \
	block_decompress  \
	dictionary_round_trip \
	dictionary_decompress \
	zstd_frame_info \
	simple_compress \
	dictionary_loader

all: $(FUZZ_TARGETS)

%.o: %.c
	$(CC) $(FUZZ_CPPFLAGS) $(FUZZ_CFLAGS) $^ -c -o $@

simple_round_trip: $(FUZZ_HEADERS) $(FUZZ_OBJ) simple_round_trip.o
	$(CXX) $(FUZZ_TARGET_FLAGS) $(FUZZ_OBJ) simple_round_trip.o $(LIB_FUZZING_ENGINE) -o $@

stream_round_trip: $(FUZZ_HEADERS) $(FUZZ_OBJ) stream_round_trip.o
	$(CXX) $(FUZZ_TARGET_FLAGS) $(FUZZ_OBJ) stream_round_trip.o $(LIB_FUZZING_ENGINE) -o $@

block_round_trip: $(FUZZ_HEADERS) $(FUZZ_OBJ) block_round_trip.o
	$(CXX) $(FUZZ_TARGET_FLAGS) $(FUZZ_OBJ) block_round_trip.o $(LIB_FUZZING_ENGINE) -o $@

simple_decompress: $(FUZZ_HEADERS) $(FUZZ_OBJ) simple_decompress.o
	$(CXX) $(FUZZ_TARGET_FLAGS) $(FUZZ_OBJ) simple_decompress.o $(LIB_FUZZING_ENGINE) -o $@

stream_decompress: $(FUZZ_HEADERS) $(FUZZ_OBJ) stream_decompress.o
	$(CXX) $(FUZZ_TARGET_FLAGS) $(FUZZ_OBJ) stream_decompress.o $(LIB_FUZZING_ENGINE) -o $@

block_decompress: $(FUZZ_HEADERS) $(FUZZ_OBJ) block_decompress.o
	$(CXX) $(FUZZ_TARGET_FLAGS) $(FUZZ_OBJ) block_decompress.o $(LIB_FUZZING_ENGINE) -o $@

dictionary_round_trip: $(FUZZ_HEADERS) $(FUZZ_OBJ) dictionary_round_trip.o
	$(CXX) $(FUZZ_TARGET_FLAGS) $(FUZZ_OBJ) dictionary_round_trip.o $(LIB_FUZZING_ENGINE) -o $@

dictionary_decompress: $(FUZZ_HEADERS) $(FUZZ_OBJ) dictionary_decompress.o
	$(CXX) $(FUZZ_TARGET_FLAGS) $(FUZZ_OBJ) dictionary_decompress.o $(LIB_FUZZING_ENGINE) -o $@

simple_compress: $(FUZZ_HEADERS) $(FUZZ_OBJ) simple_compress.o
	$(CXX) $(FUZZ_TARGET_FLAGS) $(FUZZ_OBJ) simple_compress.o $(LIB_FUZZING_ENGINE) -o $@

zstd_frame_info: $(FUZZ_HEADERS) $(FUZZ_OBJ) zstd_frame_info.o
	$(CXX) $(FUZZ_TARGET_FLAGS) $(FUZZ_OBJ) zstd_frame_info.o $(LIB_FUZZING_ENGINE) -o $@

dictionary_loader: $(FUZZ_HEADERS) $(FUZZ_OBJ) dictionary_loader.o
	$(CXX) $(FUZZ_TARGET_FLAGS) $(FUZZ_OBJ) dictionary_loader.o $(LIB_FUZZING_ENGINE) -o $@

libregression.a: $(FUZZ_HEADERS) $(PRGDIR)/util.h $(PRGDIR)/util.c regression_driver.o
	$(AR) $(FUZZ_ARFLAGS) $@ regression_driver.o

corpora/%_seed_corpus.zip:
	@mkdir -p corpora
	$(DOWNLOAD) $@ $(CORPORA_URL_PREFIX)$*_seed_corpus.zip

corpora/%: corpora/%_seed_corpus.zip
	unzip -q $^ -d $@

.PHONY: corpora
corpora: $(patsubst %,corpora/%,$(FUZZ_TARGETS))

.PHONY: seedcorpora
seedcorpora: $(patsubst %,corpora/%_seed_corpus.zip,$(FUZZ_TARGETS))

regressiontest: corpora
	CC="$(CC)" CXX="$(CXX)" CFLAGS="$(CFLAGS)" CXXFLAGS="$(CXXFLAGS)" LDFLAGS="$(LDFLAGS)" $(PYTHON) ./fuzz.py build all
	$(PYTHON) ./fuzz.py regression all

clean:
	@$(MAKE) -C $(ZSTDDIR) clean
	@$(RM) *.a *.o
	@$(RM) simple_round_trip stream_round_trip simple_decompress \
           stream_decompress block_decompress block_round_trip \
           simple_compress dictionary_round_trip dictionary_decompress \
           zstd_frame_info

cleanall:
	@$(RM) -r Fuzzer
	@$(RM) -r corpora
