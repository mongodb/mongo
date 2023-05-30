# ################################################################
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# You may select, at your option, one of the above-listed licenses.
# ################################################################

# verbose mode (print commands) on V=1 or VERBOSE=1
Q = $(if $(filter 1,$(V) $(VERBOSE)),,@)

PRGDIR   = programs
ZSTDDIR  = lib
BUILDIR  = build
ZWRAPDIR = zlibWrapper
TESTDIR  = tests
FUZZDIR  = $(TESTDIR)/fuzz

# Define nul output
VOID = /dev/null

# When cross-compiling from linux to windows, you might
# need to specify this as "Windows." Fedora build fails
# without it.
#
# Note: mingw-w64 build from linux to windows does not
# fail on other tested distros (ubuntu, debian) even
# without manually specifying the TARGET_SYSTEM.
TARGET_SYSTEM ?= $(OS)
CP ?= cp

ifneq (,$(filter Windows%,$(TARGET_SYSTEM)))
  EXT =.exe
else
  EXT =
endif

## default: Build lib-release and zstd-release
.PHONY: default
default: lib-release zstd-release

.PHONY: all
all: allmost examples manual contrib

.PHONY: allmost
allmost: allzstd zlibwrapper

# skip zwrapper, can't build that on alternate architectures without the proper zlib installed
.PHONY: allzstd
allzstd: lib
	$(Q)$(MAKE) -C $(PRGDIR) all
	$(Q)$(MAKE) -C $(TESTDIR) all

.PHONY: all32
all32:
	$(MAKE) -C $(PRGDIR) zstd32
	$(MAKE) -C $(TESTDIR) all32

.PHONY: lib lib-release lib-mt lib-nomt
lib lib-release lib-mt lib-nomt:
	$(Q)$(MAKE) -C $(ZSTDDIR) $@

.PHONY: zstd zstd-release
zstd zstd-release:
	$(Q)$(MAKE) -C $(PRGDIR) $@
	$(Q)ln -sf $(PRGDIR)/zstd$(EXT) zstd$(EXT)

.PHONY: zstdmt
zstdmt:
	$(Q)$(MAKE) -C $(PRGDIR) $@
	$(Q)$(CP) $(PRGDIR)/zstd$(EXT) ./zstdmt$(EXT)

.PHONY: zlibwrapper
zlibwrapper: lib
	$(MAKE) -C $(ZWRAPDIR) all

## test: run long-duration tests
.PHONY: test
DEBUGLEVEL ?= 1
test: MOREFLAGS += -g -Werror
test:
	DEBUGLEVEL=$(DEBUGLEVEL) MOREFLAGS="$(MOREFLAGS)" $(MAKE) -j -C $(PRGDIR) allVariants
	$(MAKE) -C $(TESTDIR) $@
	ZSTD=../../programs/zstd $(MAKE) -C doc/educational_decoder $@

## shortest: same as `make check`
.PHONY: shortest
shortest:
	$(Q)$(MAKE) -C $(TESTDIR) $@

## check: run basic tests for `zstd` cli
.PHONY: check
check: shortest

.PHONY: automated_benchmarking
automated_benchmarking:
	$(MAKE) -C $(TESTDIR) $@

.PHONY: benchmarking
benchmarking: automated_benchmarking

## examples: build all examples in `examples/` directory
.PHONY: examples
examples: lib
	$(MAKE) -C examples all

## manual: generate API documentation in html format
.PHONY: manual
manual:
	$(MAKE) -C contrib/gen_html $@

## man: generate man page
.PHONY: man
man:
	$(MAKE) -C programs $@

## contrib: build all supported projects in `/contrib` directory
.PHONY: contrib
contrib: lib
	$(MAKE) -C contrib/pzstd all
	$(MAKE) -C contrib/seekable_format/examples all
	$(MAKE) -C contrib/seekable_format/tests test
	$(MAKE) -C contrib/largeNbDicts all
	$(MAKE) -C contrib/externalSequenceProducer all
	cd build/single_file_libs/ ; ./build_decoder_test.sh
	cd build/single_file_libs/ ; ./build_library_test.sh

.PHONY: cleanTabs
cleanTabs:
	cd contrib; ./cleanTabs

.PHONY: clean
clean:
	$(Q)$(MAKE) -C $(ZSTDDIR) $@ > $(VOID)
	$(Q)$(MAKE) -C $(PRGDIR) $@ > $(VOID)
	$(Q)$(MAKE) -C $(TESTDIR) $@ > $(VOID)
	$(Q)$(MAKE) -C $(ZWRAPDIR) $@ > $(VOID)
	$(Q)$(MAKE) -C examples/ $@ > $(VOID)
	$(Q)$(MAKE) -C contrib/gen_html $@ > $(VOID)
	$(Q)$(MAKE) -C contrib/pzstd $@ > $(VOID)
	$(Q)$(MAKE) -C contrib/seekable_format/examples $@ > $(VOID)
	$(Q)$(MAKE) -C contrib/seekable_format/tests $@ > $(VOID)
	$(Q)$(MAKE) -C contrib/largeNbDicts $@ > $(VOID)
	$(Q)$(MAKE) -C contrib/externalSequenceProducer $@ > $(VOID)
	$(Q)$(RM) zstd$(EXT) zstdmt$(EXT) tmp*
	$(Q)$(RM) -r lz4
	@echo Cleaning completed

#------------------------------------------------------------------------------
# make install is validated only for Linux, macOS, Hurd and some BSD targets
#------------------------------------------------------------------------------
ifneq (,$(filter $(shell uname),Linux Darwin GNU/kFreeBSD GNU OpenBSD FreeBSD DragonFly NetBSD MSYS_NT Haiku AIX))

HOST_OS = POSIX

MKDIR ?= mkdir -p

HAVE_COLORNEVER = $(shell echo a | egrep --color=never a > /dev/null 2> /dev/null && echo 1 || echo 0)
EGREP_OPTIONS ?=
ifeq ($(HAVE_COLORNEVER), 1)
EGREP_OPTIONS += --color=never
endif
EGREP = egrep $(EGREP_OPTIONS)

# Print a two column output of targets and their description. To add a target description, put a
# comment in the Makefile with the format "## <TARGET>: <DESCRIPTION>".  For example:
#
## list: Print all targets and their descriptions (if provided)
.PHONY: list
list:
	$(Q)TARGETS=$$($(MAKE) -pRrq -f $(lastword $(MAKEFILE_LIST)) : 2>/dev/null \
		| awk -v RS= -F: '/^# File/,/^# Finished Make data base/ {if ($$1 !~ "^[#.]") {print $$1}}' \
		| $(EGREP) -v  -e '^[^[:alnum:]]' | sort); \
	{ \
	    printf "Target Name\tDescription\n"; \
	    printf "%0.s-" {1..16}; printf "\t"; printf "%0.s-" {1..40}; printf "\n"; \
	    for target in $$TARGETS; do \
	        line=$$($(EGREP) "^##[[:space:]]+$$target:" $(lastword $(MAKEFILE_LIST))); \
	        description=$$(echo $$line | awk '{i=index($$0,":"); print substr($$0,i+1)}' | xargs); \
	        printf "$$target\t$$description\n"; \
	    done \
	} | column -t -s $$'\t'

.PHONY: install armtest usan asan uasan msan asan32
install:
	$(Q)$(MAKE) -C $(ZSTDDIR) $@
	$(Q)$(MAKE) -C $(PRGDIR) $@

.PHONY: uninstall
uninstall:
	$(Q)$(MAKE) -C $(ZSTDDIR) $@
	$(Q)$(MAKE) -C $(PRGDIR) $@

.PHONY: travis-install
travis-install:
	$(MAKE) install PREFIX=~/install_test_dir

.PHONY: gcc5build gcc6build gcc7build clangbuild m32build armbuild aarch64build ppcbuild ppc64build
gcc5build: clean
	gcc-5 -v
	CC=gcc-5 $(MAKE) all MOREFLAGS="-Werror $(MOREFLAGS)"

gcc6build: clean
	gcc-6 -v
	CC=gcc-6 $(MAKE) all MOREFLAGS="-Werror $(MOREFLAGS)"

gcc7build: clean
	gcc-7 -v
	CC=gcc-7 $(MAKE) all MOREFLAGS="-Werror $(MOREFLAGS)"

clangbuild: clean
	clang -v
	CXX=clang++ CC=clang CFLAGS="-Werror -Wconversion -Wno-sign-conversion -Wdocumentation" $(MAKE) all

m32build: clean
	gcc -v
	$(MAKE) all32

armbuild: clean
	CC=arm-linux-gnueabi-gcc CFLAGS="-Werror" $(MAKE) allzstd

aarch64build: clean
	CC=aarch64-linux-gnu-gcc CFLAGS="-Werror -O0" $(MAKE) allzstd

ppcbuild: clean
	CC=powerpc-linux-gnu-gcc CFLAGS="-m32 -Wno-attributes -Werror" $(MAKE) -j allzstd

ppc64build: clean
	CC=powerpc-linux-gnu-gcc CFLAGS="-m64 -Werror" $(MAKE) -j allzstd

.PHONY: armfuzz aarch64fuzz ppcfuzz ppc64fuzz
armfuzz: clean
	CC=arm-linux-gnueabi-gcc QEMU_SYS=qemu-arm-static MOREFLAGS="-static $(MOREFLAGS)" FUZZER_FLAGS="--no-big-tests $(FUZZER_FLAGS)" $(MAKE) -C $(TESTDIR) fuzztest

aarch64fuzz: clean
	ld -v
	CC=aarch64-linux-gnu-gcc QEMU_SYS=qemu-aarch64-static MOREFLAGS="-static $(MOREFLAGS)" FUZZER_FLAGS="--no-big-tests $(FUZZER_FLAGS)" $(MAKE) -C $(TESTDIR) fuzztest

ppcfuzz: clean
	CC=powerpc-linux-gnu-gcc QEMU_SYS=qemu-ppc-static MOREFLAGS="-static $(MOREFLAGS)" FUZZER_FLAGS="--no-big-tests $(FUZZER_FLAGS)" $(MAKE) -C $(TESTDIR) fuzztest

ppc64fuzz: clean
	CC=powerpc-linux-gnu-gcc QEMU_SYS=qemu-ppc64-static MOREFLAGS="-m64 -static $(MOREFLAGS)" FUZZER_FLAGS="--no-big-tests $(FUZZER_FLAGS)" $(MAKE) -C $(TESTDIR) fuzztest

.PHONY: cxxtest gcc5test gcc6test armtest aarch64test ppctest ppc64test
cxxtest: CXXFLAGS += -Wall -Wextra -Wundef -Wshadow -Wcast-align -Werror
cxxtest: clean
	$(MAKE) -C $(PRGDIR) all CC="$(CXX) -Wno-deprecated" CFLAGS="$(CXXFLAGS)"   # adding -Wno-deprecated to avoid clang++ warning on dealing with C files directly

gcc5test: clean
	gcc-5 -v
	$(MAKE) all CC=gcc-5 MOREFLAGS="-Werror $(MOREFLAGS)"

gcc6test: clean
	gcc-6 -v
	$(MAKE) all CC=gcc-6 MOREFLAGS="-Werror $(MOREFLAGS)"

armtest: clean
	$(MAKE) -C $(TESTDIR) datagen   # use native, faster
	$(MAKE) -C $(TESTDIR) test CC=arm-linux-gnueabi-gcc QEMU_SYS=qemu-arm-static ZSTDRTTEST= MOREFLAGS="-Werror -static $(MOREFLAGS)" FUZZER_FLAGS="--no-big-tests $(FUZZER_FLAGS)"

aarch64test:
	$(MAKE) -C $(TESTDIR) datagen   # use native, faster
	$(MAKE) -C $(TESTDIR) test CC=aarch64-linux-gnu-gcc QEMU_SYS=qemu-aarch64-static ZSTDRTTEST= MOREFLAGS="-Werror -static $(MOREFLAGS)" FUZZER_FLAGS="--no-big-tests $(FUZZER_FLAGS)"

ppctest: clean
	$(MAKE) -C $(TESTDIR) datagen   # use native, faster
	$(MAKE) -C $(TESTDIR) test CC=powerpc-linux-gnu-gcc QEMU_SYS=qemu-ppc-static ZSTDRTTEST= MOREFLAGS="-Werror -Wno-attributes -static $(MOREFLAGS)" FUZZER_FLAGS="--no-big-tests $(FUZZER_FLAGS)"

ppc64test: clean
	$(MAKE) -C $(TESTDIR) datagen   # use native, faster
	$(MAKE) -C $(TESTDIR) test CC=powerpc-linux-gnu-gcc QEMU_SYS=qemu-ppc64-static ZSTDRTTEST= MOREFLAGS="-m64 -static $(MOREFLAGS)" FUZZER_FLAGS="--no-big-tests $(FUZZER_FLAGS)"

.PHONY: arm-ppc-compilation
arm-ppc-compilation:
	$(MAKE) -C $(PRGDIR) clean zstd CC=arm-linux-gnueabi-gcc QEMU_SYS=qemu-arm-static ZSTDRTTEST= MOREFLAGS="-Werror -static $(MOREFLAGS)"
	$(MAKE) -C $(PRGDIR) clean zstd CC=aarch64-linux-gnu-gcc QEMU_SYS=qemu-aarch64-static ZSTDRTTEST= MOREFLAGS="-Werror -static $(MOREFLAGS)"
	$(MAKE) -C $(PRGDIR) clean zstd CC=powerpc-linux-gnu-gcc QEMU_SYS=qemu-ppc-static ZSTDRTTEST= MOREFLAGS="-Werror -Wno-attributes -static $(MOREFLAGS)"
	$(MAKE) -C $(PRGDIR) clean zstd CC=powerpc-linux-gnu-gcc QEMU_SYS=qemu-ppc64-static ZSTDRTTEST= MOREFLAGS="-m64 -static $(MOREFLAGS)"

regressiontest:
	$(MAKE) -C $(FUZZDIR) regressiontest

uasanregressiontest:
	$(MAKE) -C $(FUZZDIR) regressiontest CC=clang CXX=clang++ CFLAGS="-O3 -fsanitize=address,undefined" CXXFLAGS="-O3 -fsanitize=address,undefined"

msanregressiontest:
	$(MAKE) -C $(FUZZDIR) regressiontest CC=clang CXX=clang++ CFLAGS="-O3 -fsanitize=memory" CXXFLAGS="-O3 -fsanitize=memory"

update_regressionResults : REGRESS_RESULTS_DIR := /tmp/regress_results_dir/
update_regressionResults:
	$(MAKE) -C programs zstd
	$(MAKE) -C tests/regression test
	$(RM) -rf $(REGRESS_RESULTS_DIR)
	$(MKDIR) $(REGRESS_RESULTS_DIR)
	./tests/regression/test                         \
        --cache  tests/regression/cache             \
        --output $(REGRESS_RESULTS_DIR)/results.csv \
        --zstd   programs/zstd
	echo "Showing results differences"
	! diff tests/regression/results.csv $(REGRESS_RESULTS_DIR)/results.csv
	echo "Updating results.csv"
	$(CP) $(REGRESS_RESULTS_DIR)/results.csv tests/regression/results.csv


# run UBsan with -fsanitize-recover=pointer-overflow
# this only works with recent compilers such as gcc 8+
usan: clean
	$(MAKE) test CC=clang MOREFLAGS="-g -fno-sanitize-recover=all -fsanitize-recover=pointer-overflow -fsanitize=undefined -Werror $(MOREFLAGS)"

asan: clean
	$(MAKE) test CC=clang MOREFLAGS="-g -fsanitize=address -Werror $(MOREFLAGS)"

asan-%: clean
	LDFLAGS=-fuse-ld=gold MOREFLAGS="-g -fno-sanitize-recover=all -fsanitize=address -Werror $(MOREFLAGS)" $(MAKE) -C $(TESTDIR) $*

msan: clean
	$(MAKE) test CC=clang MOREFLAGS="-g -fsanitize=memory -fno-omit-frame-pointer -Werror $(MOREFLAGS)" HAVE_LZMA=0   # datagen.c fails this test for no obvious reason

msan-%: clean
	LDFLAGS=-fuse-ld=gold MOREFLAGS="-g -fno-sanitize-recover=all -fsanitize=memory -fno-omit-frame-pointer -Werror $(MOREFLAGS)" FUZZER_FLAGS="--no-big-tests $(FUZZER_FLAGS)" $(MAKE) -C $(TESTDIR) HAVE_LZMA=0 $*

asan32: clean
	$(MAKE) -C $(TESTDIR) test32 CC=clang MOREFLAGS="-g -fsanitize=address $(MOREFLAGS)"

uasan: clean
	$(MAKE) test CC=clang MOREFLAGS="-g -fno-sanitize-recover=all -fsanitize-recover=pointer-overflow -fsanitize=address,undefined -Werror $(MOREFLAGS)"

uasan-%: clean
	LDFLAGS=-fuse-ld=gold MOREFLAGS="-g -fno-sanitize-recover=all -fsanitize-recover=pointer-overflow -fsanitize=address,undefined -Werror $(MOREFLAGS)" $(MAKE) -C $(TESTDIR) $*

tsan-%: clean
	LDFLAGS=-fuse-ld=gold MOREFLAGS="-g -fno-sanitize-recover=all -fsanitize=thread -Werror $(MOREFLAGS)" $(MAKE) -C $(TESTDIR) $* FUZZER_FLAGS="--no-big-tests $(FUZZER_FLAGS)"

.PHONY: apt-install
apt-install:
	# TODO: uncomment once issue 3011 is resolved and remove hack from Github Actions .yml
	# sudo apt-get update
	sudo apt-get -yq --no-install-suggests --no-install-recommends --force-yes install $(APT_PACKAGES)

.PHONY: apt-add-repo
apt-add-repo:
	sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
	sudo apt-get update -y -qq

.PHONY: ppcinstall arminstall valgrindinstall libc6install gcc6install gcc7install gcc8install gpp6install clang38install lz4install
ppcinstall:
	APT_PACKAGES="qemu-system-ppc qemu-user-static gcc-powerpc-linux-gnu" $(MAKE) apt-install

arminstall:
	APT_PACKAGES="qemu-system-arm qemu-user-static gcc-arm-linux-gnueabi libc6-dev-armel-cross gcc-aarch64-linux-gnu libc6-dev-arm64-cross" $(MAKE) apt-install

valgrindinstall:
	APT_PACKAGES="valgrind" $(MAKE) apt-install

libc6install:
	APT_PACKAGES="libc6-dev-i386 gcc-multilib" $(MAKE) apt-install

gcc6install: apt-add-repo
	APT_PACKAGES="libc6-dev-i386 gcc-multilib gcc-6 gcc-6-multilib" $(MAKE) apt-install

gcc7install: apt-add-repo
	APT_PACKAGES="libc6-dev-i386 gcc-multilib gcc-7 gcc-7-multilib" $(MAKE) apt-install

gcc8install: apt-add-repo
	APT_PACKAGES="libc6-dev-i386 gcc-multilib gcc-8 gcc-8-multilib" $(MAKE) apt-install

gpp6install: apt-add-repo
	APT_PACKAGES="libc6-dev-i386 g++-multilib gcc-6 g++-6 g++-6-multilib" $(MAKE) apt-install

clang38install:
	APT_PACKAGES="clang-3.8" $(MAKE) apt-install

# Ubuntu 14.04 ships a too-old lz4
lz4install:
	[ -e lz4 ] || git clone https://github.com/lz4/lz4 && sudo $(MAKE) -C lz4 install

endif


CMAKE_PARAMS = -DZSTD_BUILD_CONTRIB:BOOL=ON -DZSTD_BUILD_STATIC:BOOL=ON -DZSTD_BUILD_TESTS:BOOL=ON -DZSTD_ZLIB_SUPPORT:BOOL=ON -DZSTD_LZMA_SUPPORT:BOOL=ON -DCMAKE_BUILD_TYPE=Release

ifneq (,$(filter MSYS%,$(shell uname)))
HOST_OS = MSYS
CMAKE_PARAMS = -G"MSYS Makefiles" -DCMAKE_BUILD_TYPE=Debug -DZSTD_MULTITHREAD_SUPPORT:BOOL=OFF -DZSTD_BUILD_STATIC:BOOL=ON -DZSTD_BUILD_TESTS:BOOL=ON
endif

#------------------------------------------------------------------------
# target specific tests
#------------------------------------------------------------------------
ifneq (,$(filter $(HOST_OS),MSYS POSIX))
.PHONY: cmakebuild c89build gnu90build c99build gnu99build c11build bmix64build bmix32build bmi32build staticAnalyze
cmakebuild:
	cmake --version
	$(RM) -r $(BUILDIR)/cmake/build
	$(MKDIR) $(BUILDIR)/cmake/build
	cd $(BUILDIR)/cmake/build; cmake -DCMAKE_INSTALL_PREFIX:PATH=~/install_test_dir $(CMAKE_PARAMS) ..
	$(MAKE) -C $(BUILDIR)/cmake/build -j4;
	$(MAKE) -C $(BUILDIR)/cmake/build install;
	$(MAKE) -C $(BUILDIR)/cmake/build uninstall;
	cd $(BUILDIR)/cmake/build; ctest -V -L Medium

c89build: clean
	$(CC) -v
	CFLAGS="-std=c89 -Werror -Wno-attributes -Wpedantic -Wno-long-long -Wno-variadic-macros -O0" $(MAKE) lib zstd

gnu90build: clean
	$(CC) -v
	CFLAGS="-std=gnu90 -Werror -O0" $(MAKE) allmost

c99build: clean
	$(CC) -v
	CFLAGS="-std=c99 -Werror -O0" $(MAKE) allmost

gnu99build: clean
	$(CC) -v
	CFLAGS="-std=gnu99 -Werror -O0" $(MAKE) allmost

c11build: clean
	$(CC) -v
	CFLAGS="-std=c11 -Werror -O0" $(MAKE) allmost

bmix64build: clean
	$(CC) -v
	CFLAGS="-O3 -mbmi -Werror" $(MAKE) -C $(TESTDIR) test

bmix32build: clean
	$(CC) -v
	CFLAGS="-O3 -mbmi -mx32 -Werror" $(MAKE) -C $(TESTDIR) test

bmi32build: clean
	$(CC) -v
	CFLAGS="-O3 -mbmi -m32 -Werror" $(MAKE) -C $(TESTDIR) test

# static analyzer test uses clang's scan-build
# does not analyze zlibWrapper, due to detected issues in zlib source code
staticAnalyze: SCANBUILD ?= scan-build
staticAnalyze:
	$(CC) -v
	CC=$(CC) CPPFLAGS=-g $(SCANBUILD) --status-bugs -v $(MAKE) zstd
endif
