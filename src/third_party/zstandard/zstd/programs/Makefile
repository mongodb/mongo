# ################################################################
# Copyright (c) Yann Collet, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# You may select, at your option, one of the above-listed licenses.
# ##########################################################################
# zstd : Command Line Utility, supporting gzip-like arguments
# zstd32 : Same as zstd, but forced to compile in 32-bits mode
# zstd-nolegacy : zstd without support of decompression of legacy versions
# zstd-small : minimal zstd without dictionary builder and benchmark
# zstd-compress : compressor-only version of zstd
# zstd-decompress : decompressor-only version of zstd
# ##########################################################################

.PHONY: default
default: zstd-release

LIBZSTD := ../lib

include $(LIBZSTD)/libzstd.mk

ifeq ($(shell $(CC) -v 2>&1 | $(GREP) -c "gcc version "), 1)
  ALIGN_LOOP = -falign-loops=32
else
  ALIGN_LOOP =
endif

ZSTDLIB_COMMON_SRC := $(sort $(ZSTD_COMMON_FILES))
ZSTDLIB_COMPRESS_SRC := $(sort $(ZSTD_COMPRESS_FILES))
ZSTDLIB_DECOMPRESS_SRC := $(sort $(ZSTD_DECOMPRESS_FILES))
ZSTDLIB_CORE_SRC := $(sort $(ZSTD_DECOMPRESS_FILES) $(ZSTD_COMMON_FILES) $(ZSTD_COMPRESS_FILES))
ZDICT_SRC := $(sort $(ZSTD_DICTBUILDER_FILES))
ZSTDLEGACY_SRC := $(sort $(ZSTD_LEGACY_FILES))

# Sort files in alphabetical order for reproducible builds
ZSTDLIB_FULL_SRC = $(sort $(ZSTDLIB_CORE_SRC) $(ZSTDLEGACY_SRC) $(ZDICT_SRC))
ZSTDLIB_LOCAL_SRC = $(notdir $(ZSTDLIB_FULL_SRC))
ZSTDLIB_LOCAL_OBJ0 := $(ZSTDLIB_LOCAL_SRC:.c=.o)
ZSTDLIB_LOCAL_OBJ := $(ZSTDLIB_LOCAL_OBJ0:.S=.o)

ZSTD_CLI_SRC := $(sort $(wildcard *.c))
ZSTD_CLI_OBJ := $(ZSTD_CLI_SRC:.c=.o)

ZSTD_ALL_SRC = $(ZSTDLIB_LOCAL_SRC) $(ZSTD_CLI_SRC)
ZSTD_ALL_OBJ0 := $(ZSTD_ALL_SRC:.c=.o)
ZSTD_ALL_OBJ := $(ZSTD_ALL_OBJ0:.S=.o)

# Define *.exe as extension for Windows systems
ifneq (,$(filter Windows%,$(OS)))
  EXT =.exe
  RES64_FILE = windres/zstd64.res
  RES32_FILE = windres/zstd32.res
ifneq (,$(filter x86_64%,$(shell $(CC) -dumpmachine)))
    RES_FILE = $(RES64_FILE)
else
    RES_FILE = $(RES32_FILE)
endif
else
  EXT =
endif

# thread detection
NO_THREAD_MSG := ==> no threads, building without multithreading support
HAVE_PTHREAD := $(shell printf '$(NUM_SYMBOL)include <pthread.h>\nint main(void) { return 0; }' > have_pthread.c && $(CC) $(FLAGS) -o have_pthread$(EXT) have_pthread.c -pthread 2> $(VOID) && rm have_pthread$(EXT) && echo 1 || echo 0; rm have_pthread.c)
HAVE_THREAD := $(shell [ "$(HAVE_PTHREAD)" -eq "1" -o -n "$(filter Windows%,$(OS))" ] && echo 1 || echo 0)
ifeq ($(HAVE_THREAD), 1)
  THREAD_MSG := ==> building with threading support
  THREAD_CPP := -DZSTD_MULTITHREAD
  THREAD_LD := -pthread
else
  THREAD_MSG := $(NO_THREAD_MSG)
endif

# zlib detection
NO_ZLIB_MSG := ==> no zlib, building zstd without .gz support
HAVE_ZLIB ?= $(shell printf '$(NUM_SYMBOL)include <zlib.h>\nint main(void) { return 0; }' > have_zlib.c && $(CC) $(FLAGS) -o have_zlib$(EXT) have_zlib.c -lz 2> $(VOID) && rm have_zlib$(EXT) && echo 1 || echo 0; rm have_zlib.c)
ifeq ($(HAVE_ZLIB), 1)
  ZLIB_MSG := ==> building zstd with .gz compression support
  ZLIBCPP = -DZSTD_GZCOMPRESS -DZSTD_GZDECOMPRESS
  ZLIBLD = -lz
else
  ZLIB_MSG := $(NO_ZLIB_MSG)
endif

# lzma detection
NO_LZMA_MSG := ==> no liblzma, building zstd without .xz/.lzma support
HAVE_LZMA ?= $(shell printf '$(NUM_SYMBOL)include <lzma.h>\nint main(void) { return 0; }' > have_lzma.c && $(CC) $(FLAGS) -o have_lzma$(EXT) have_lzma.c -llzma 2> $(VOID) && rm have_lzma$(EXT) && echo 1 || echo 0; rm have_lzma.c)
ifeq ($(HAVE_LZMA), 1)
  LZMA_MSG := ==> building zstd with .xz/.lzma compression support
  LZMACPP = -DZSTD_LZMACOMPRESS -DZSTD_LZMADECOMPRESS
  LZMALD = -llzma
else
  LZMA_MSG := $(NO_LZMA_MSG)
endif

# lz4 detection
NO_LZ4_MSG := ==> no liblz4, building zstd without .lz4 support
HAVE_LZ4 ?= $(shell printf '$(NUM_SYMBOL)include <lz4frame.h>\n$(NUM_SYMBOL)include <lz4.h>\nint main(void) { return 0; }' > have_lz4.c && $(CC) $(FLAGS) -o have_lz4$(EXT) have_lz4.c -llz4 2> $(VOID) && rm have_lz4$(EXT) && echo 1 || echo 0; rm have_lz4.c)
ifeq ($(HAVE_LZ4), 1)
  LZ4_MSG := ==> building zstd with .lz4 compression support
  LZ4CPP = -DZSTD_LZ4COMPRESS -DZSTD_LZ4DECOMPRESS
  LZ4LD = -llz4
else
  LZ4_MSG := $(NO_LZ4_MSG)
endif

# explicit backtrace enable/disable for Linux & Darwin
ifeq ($(BACKTRACE), 0)
  DEBUGFLAGS += -DBACKTRACE_ENABLE=0
endif
ifeq (,$(filter Windows%, $(OS)))
ifeq ($(BACKTRACE), 1)
  DEBUGFLAGS += -DBACKTRACE_ENABLE=1
  DEBUGFLAGS_LD += -rdynamic
endif
endif

SET_CACHE_DIRECTORY = \
   +$(MAKE) --no-print-directory $@ \
    BUILD_DIR=obj/$(HASH_DIR) \
    CPPFLAGS="$(CPPFLAGS)" \
    CFLAGS="$(CFLAGS)" \
    LDFLAGS="$(LDFLAGS)" \
    LDLIBS="$(LDLIBS)" \
    ZSTD_ALL_SRC="$(ZSTD_ALL_SRC)"


.PHONY: all
all: zstd

.PHONY: allVariants
allVariants: zstd zstd-compress zstd-decompress zstd-small zstd-frugal zstd-nolegacy zstd-dictBuilder

.PHONY: zstd  # must always be run
zstd : CPPFLAGS += $(THREAD_CPP) $(ZLIBCPP) $(LZMACPP) $(LZ4CPP)
zstd : LDFLAGS += $(THREAD_LD) $(DEBUGFLAGS_LD)
zstd : LDLIBS += $(ZLIBLD) $(LZMALD) $(LZ4LD)
zstd : CPPFLAGS += -DZSTD_LEGACY_SUPPORT=$(ZSTD_LEGACY_SUPPORT)
ifneq (,$(filter Windows%,$(OS)))
zstd : $(RES_FILE)
endif

ifndef BUILD_DIR
# generate BUILD_DIR from flags

zstd:
	$(SET_CACHE_DIRECTORY)

else
# BUILD_DIR is defined

ZSTD_OBJ := $(addprefix $(BUILD_DIR)/, $(ZSTD_ALL_OBJ))
$(BUILD_DIR)/zstd : $(ZSTD_OBJ)
	@echo "$(THREAD_MSG)"
	@echo "$(ZLIB_MSG)"
	@echo "$(LZMA_MSG)"
	@echo "$(LZ4_MSG)"
	@echo LINK $@
	$(CC) $(FLAGS) $^ $(LDLIBS) -o $@$(EXT)

ifeq ($(HAVE_HASH),1)
SRCBIN_HASH = $(shell cat $(BUILD_DIR)/zstd$(EXT) 2> $(VOID) | $(HASH) | cut -f 1 -d " ")
DSTBIN_HASH = $(shell cat zstd$(EXT) 2> $(VOID) | $(HASH) | cut -f 1 -d " ")
BIN_ISDIFFERENT = $(if $(filter $(SRCBIN_HASH),$(DSTBIN_HASH)),0,1)
else
BIN_ISDIFFERENT = 1
endif

zstd : $(BUILD_DIR)/zstd
	if [ $(BIN_ISDIFFERENT) -eq 1 ]; then \
		cp -f $<$(EXT) $@$(EXT); \
		echo zstd build completed; \
	else \
		echo zstd already built; \
	fi

endif  # BUILD_DIR


.PHONY: zstd-release
zstd-release: DEBUGFLAGS := -DBACKTRACE_ENABLE=0
zstd-release: DEBUGFLAGS_LD :=
zstd-release: zstd

zstd32 : CPPFLAGS += $(THREAD_CPP)
zstd32 : LDFLAGS  += $(THREAD_LD)
zstd32 : CPPFLAGS += -DZSTD_LEGACY_SUPPORT=$(ZSTD_LEGACY_SUPPORT)
ifneq (,$(filter Windows%,$(OS)))
zstd32 : $(RES32_FILE)
endif
zstd32 : $(ZSTDLIB_FULL_SRC) $(ZSTD_CLI_SRC)
	$(CC) -m32 $(FLAGS) $^ -o $@$(EXT)

## zstd-nolegacy: same scope as zstd, with just support of legacy formats removed
zstd-nolegacy : LDFLAGS += $(THREAD_LD) $(ZLIBLD) $(LZMALD) $(LZ4LD) $(DEBUGFLAGS_LD)
zstd-nolegacy : CPPFLAGS += -UZSTD_LEGACY_SUPPORT -DZSTD_LEGACY_SUPPORT=0
zstd-nolegacy : $(ZSTDLIB_CORE_SRC) $(ZDICT_SRC) $(ZSTD_CLI_OBJ)
	$(CC) $(FLAGS) $^ -o $@$(EXT) $(LDFLAGS)

.PHONY: zstd-nomt
zstd-nomt : THREAD_CPP :=
zstd-nomt : THREAD_LD  :=
zstd-nomt : THREAD_MSG := - multi-threading disabled
zstd-nomt : zstd

.PHONY: zstd-nogz
zstd-nogz : ZLIBCPP :=
zstd-nogz : ZLIBLD  :=
zstd-nogz : ZLIB_MSG := - gzip support is disabled
zstd-nogz : zstd

.PHONY: zstd-noxz
zstd-noxz : LZMACPP :=
zstd-noxz : LZMALD  :=
zstd-noxz : LZMA_MSG := - xz/lzma support is disabled
zstd-noxz : zstd

## zstd-dll: zstd executable linked to dynamic library libzstd (must have same version)
.PHONY: zstd-dll
zstd-dll : LDFLAGS+= -L$(LIBZSTD)
zstd-dll : LDLIBS += -lzstd
zstd-dll : ZSTDLIB_LOCAL_SRC = xxhash.c
zstd-dll : zstd


## zstd-pgo: zstd executable optimized with PGO.
.PHONY: zstd-pgo
zstd-pgo :
	$(MAKE) clean
	$(MAKE) zstd MOREFLAGS=-fprofile-generate
	./zstd -b19i1 $(PROFILE_WITH)
	./zstd -b16i1 $(PROFILE_WITH)
	./zstd -b9i2 $(PROFILE_WITH)
	./zstd -b $(PROFILE_WITH)
	./zstd -b7i2 $(PROFILE_WITH)
	./zstd -b5 $(PROFILE_WITH)
	$(RM) zstd *.o
	case $(CC) in *clang*) if ! [ -e default.profdata ]; then llvm-profdata merge -output=default.profdata default*.profraw; fi ;; esac
	$(MAKE) zstd MOREFLAGS=-fprofile-use

## zstd-small: minimal target, supporting only zstd compression and decompression. no bench. no legacy. no other format.
zstd-small: CFLAGS = -Os -s
zstd-frugal zstd-small: $(ZSTDLIB_CORE_SRC) zstdcli.c util.c timefn.c fileio.c
	$(CC) $(FLAGS) -DZSTD_NOBENCH -DZSTD_NODICT -DZSTD_NOTRACE -UZSTD_LEGACY_SUPPORT -DZSTD_LEGACY_SUPPORT=0 $^ -o $@$(EXT)

zstd-decompress: $(ZSTDLIB_COMMON_SRC) $(ZSTDLIB_DECOMPRESS_SRC) zstdcli.c util.c timefn.c fileio.c
	$(CC) $(FLAGS) -DZSTD_NOBENCH -DZSTD_NODICT -DZSTD_NOCOMPRESS -DZSTD_NOTRACE -UZSTD_LEGACY_SUPPORT -DZSTD_LEGACY_SUPPORT=0 $^ -o $@$(EXT)

zstd-compress: $(ZSTDLIB_COMMON_SRC) $(ZSTDLIB_COMPRESS_SRC) zstdcli.c util.c timefn.c fileio.c
	$(CC) $(FLAGS) -DZSTD_NOBENCH -DZSTD_NODICT -DZSTD_NODECOMPRESS -DZSTD_NOTRACE -UZSTD_LEGACY_SUPPORT -DZSTD_LEGACY_SUPPORT=0 $^ -o $@$(EXT)

## zstd-dictBuilder: executable supporting dictionary creation and compression (only)
zstd-dictBuilder: $(ZSTDLIB_COMMON_SRC) $(ZSTDLIB_COMPRESS_SRC) $(ZDICT_SRC) zstdcli.c util.c timefn.c fileio.c dibio.c
	$(CC) $(FLAGS) -DZSTD_NOBENCH -DZSTD_NODECOMPRESS -DZSTD_NOTRACE $^ -o $@$(EXT)

zstdmt: zstd
	ln -sf zstd zstdmt

.PHONY: generate_res
generate_res: $(RES64_FILE) $(RES32_FILE)

ifneq (,$(filter Windows%,$(OS)))
RC ?= windres
# http://stackoverflow.com/questions/708238/how-do-i-add-an-icon-to-a-mingw-gcc-compiled-executable
$(RES64_FILE): windres/zstd.rc
	$(RC) -o $@ -I ../lib -I windres -i $< -O coff -F pe-x86-64
$(RES32_FILE): windres/zstd.rc
	$(RC) -o $@ -I ../lib -I windres -i $< -O coff -F pe-i386
endif

.PHONY: clean
clean:
	$(RM) core *.o tmp* result* *.gcda dictionary *.zst \
        zstd$(EXT) zstd32$(EXT) zstd-dll$(EXT) \
        zstd-compress$(EXT) zstd-decompress$(EXT) \
        zstd-small$(EXT) zstd-frugal$(EXT) zstd-nolegacy$(EXT) zstd4$(EXT) \
        zstd-dictBuilder$(EXT) \
        *.gcda default*.profraw default.profdata have_zlib$(EXT)
	$(RM) -r obj/*
	@echo Cleaning completed

MD2ROFF = ronn
MD2ROFF_FLAGS = --roff --warnings --manual="User Commands" --organization="zstd $(ZSTD_VERSION)"

zstd.1: zstd.1.md ../lib/zstd.h
	cat $< | $(MD2ROFF) $(MD2ROFF_FLAGS) | sed -n '/^\.\\\".*/!p' > $@

zstdgrep.1: zstdgrep.1.md ../lib/zstd.h
	cat $< | $(MD2ROFF) $(MD2ROFF_FLAGS) | sed -n '/^\.\\\".*/!p' > $@

zstdless.1: zstdless.1.md ../lib/zstd.h
	cat $< | $(MD2ROFF) $(MD2ROFF_FLAGS) | sed -n '/^\.\\\".*/!p' > $@

.PHONY: man
man: zstd.1 zstdgrep.1 zstdless.1

.PHONY: clean-man
clean-man:
	$(RM) zstd.1
	$(RM) zstdgrep.1
	$(RM) zstdless.1

.PHONY: preview-man
preview-man: clean-man man
	man ./zstd.1
	man ./zstdgrep.1
	man ./zstdless.1


# Generate .h dependencies automatically

DEPFLAGS = -MT $@ -MMD -MP -MF

$(BUILD_DIR)/%.o : %.c $(BUILD_DIR)/%.d | $(BUILD_DIR)
	@echo CC $@
	$(COMPILE.c) $(DEPFLAGS) $(BUILD_DIR)/$*.d $(OUTPUT_OPTION) $<

$(BUILD_DIR)/%.o : %.S | $(BUILD_DIR)
	@echo AS $@
	$(COMPILE.S) $(OUTPUT_OPTION) $<

MKDIR ?= mkdir
$(BUILD_DIR): ; $(MKDIR) -p $@

DEPFILES := $(ZSTD_OBJ:.o=.d)
$(DEPFILES):

include $(wildcard $(DEPFILES))



#-----------------------------------------------------------------------------
# make install is validated only for Linux, macOS, BSD, Hurd and Solaris targets
#-----------------------------------------------------------------------------
ifneq (,$(filter $(UNAME),Linux Darwin GNU/kFreeBSD GNU OpenBSD FreeBSD NetBSD DragonFly SunOS Haiku AIX))

HAVE_COLORNEVER = $(shell echo a | egrep --color=never a > /dev/null 2> /dev/null && echo 1 || echo 0)
EGREP_OPTIONS ?=
ifeq ($HAVE_COLORNEVER, 1)
  EGREP_OPTIONS += --color=never
endif
EGREP = egrep $(EGREP_OPTIONS)
AWK = awk

# Print a two column output of targets and their description. To add a target description, put a
# comment in the Makefile with the format "## <TARGET>: <DESCRIPTION>".  For example:
#
## list: Print all targets and their descriptions (if provided)
.PHONY: list
list:
	TARGETS=$$($(MAKE) -pRrq -f $(lastword $(MAKEFILE_LIST)) : 2>/dev/null \
		| $(AWK) -v RS= -F: '/^# File/,/^# Finished Make data base/ {if ($$1 !~ "^[#.]") {print $$1}}' \
		| $(EGREP) -v  -e '^[^[:alnum:]]' | sort); \
	{ \
	    printf "Target Name\tDescription\n"; \
	    printf "%0.s-" {1..16}; printf "\t"; printf "%0.s-" {1..40}; printf "\n"; \
	    for target in $$TARGETS; do \
	        line=$$($(EGREP) "^##[[:space:]]+$$target:" $(lastword $(MAKEFILE_LIST))); \
	        description=$$(echo $$line | $(AWK) '{i=index($$0,":"); print substr($$0,i+1)}' | xargs); \
	        printf "$$target\t$$description\n"; \
	    done \
	} | column -t -s $$'\t'


DESTDIR     ?=
# directory variables : GNU conventions prefer lowercase
# see https://www.gnu.org/prep/standards/html_node/Makefile-Conventions.html
# support both lower and uppercase (BSD), use uppercase in script
prefix      ?= /usr/local
PREFIX      ?= $(prefix)
exec_prefix ?= $(PREFIX)
bindir      ?= $(exec_prefix)/bin
BINDIR      ?= $(bindir)
datarootdir ?= $(PREFIX)/share
mandir      ?= $(datarootdir)/man
man1dir     ?= $(mandir)/man1

ifneq (,$(filter $(UNAME),OpenBSD FreeBSD NetBSD DragonFly SunOS))
  MANDIR  ?= $(PREFIX)/man
  MAN1DIR ?= $(MANDIR)/man1
else
  MAN1DIR ?= $(man1dir)
endif

ifneq (,$(filter $(UNAME),SunOS))
  INSTALL ?= ginstall
else
  INSTALL ?= install
endif

INSTALL_PROGRAM ?= $(INSTALL)
INSTALL_SCRIPT  ?= $(INSTALL_PROGRAM)
INSTALL_DATA    ?= $(INSTALL) -m 644
INSTALL_MAN     ?= $(INSTALL_DATA)

.PHONY: install
install:
	# generate zstd only if not already present
	[ -e zstd ] || $(MAKE) zstd-release
	[ -e $(DESTDIR)$(BINDIR) ] || $(INSTALL) -d -m 755 $(DESTDIR)$(BINDIR)/
	[ -e $(DESTDIR)$(MAN1DIR) ] || $(INSTALL) -d -m 755 $(DESTDIR)$(MAN1DIR)/
	@echo Installing binaries
	$(INSTALL_PROGRAM) zstd$(EXT) $(DESTDIR)$(BINDIR)/zstd$(EXT)
	ln -sf zstd$(EXT) $(DESTDIR)$(BINDIR)/zstdcat$(EXT)
	ln -sf zstd$(EXT) $(DESTDIR)$(BINDIR)/unzstd$(EXT)
	ln -sf zstd$(EXT) $(DESTDIR)$(BINDIR)/zstdmt$(EXT)
	$(INSTALL_SCRIPT) zstdless $(DESTDIR)$(BINDIR)/zstdless
	$(INSTALL_SCRIPT) zstdgrep $(DESTDIR)$(BINDIR)/zstdgrep
	@echo Installing man pages
	$(INSTALL_MAN) zstd.1 $(DESTDIR)$(MAN1DIR)/zstd.1
	ln -sf zstd.1 $(DESTDIR)$(MAN1DIR)/zstdcat.1
	ln -sf zstd.1 $(DESTDIR)$(MAN1DIR)/unzstd.1
	$(INSTALL_MAN) zstdgrep.1 $(DESTDIR)$(MAN1DIR)/zstdgrep.1
	$(INSTALL_MAN) zstdless.1 $(DESTDIR)$(MAN1DIR)/zstdless.1
	@echo zstd installation completed

.PHONY: uninstall
uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/zstdgrep
	$(RM) $(DESTDIR)$(BINDIR)/zstdless
	$(RM) $(DESTDIR)$(BINDIR)/zstdcat
	$(RM) $(DESTDIR)$(BINDIR)/unzstd
	$(RM) $(DESTDIR)$(BINDIR)/zstdmt
	$(RM) $(DESTDIR)$(BINDIR)/zstd
	$(RM) $(DESTDIR)$(MAN1DIR)/zstdless.1
	$(RM) $(DESTDIR)$(MAN1DIR)/zstdgrep.1
	$(RM) $(DESTDIR)$(MAN1DIR)/zstdcat.1
	$(RM) $(DESTDIR)$(MAN1DIR)/unzstd.1
	$(RM) $(DESTDIR)$(MAN1DIR)/zstd.1
	@echo zstd programs successfully uninstalled

endif
