# THIS FILE WAS AUTOMATICALLY GENERATED. DO NOT MODIFY BY HAND.
DEPTH := ../..
topobjdir := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src
topsrcdir := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release
srcdir := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src
VPATH := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src
relativesrcdir := js/src
include $(DEPTH)/config/autoconf.mk
# -*- Mode: makefile -*-
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

ifeq (,$(MAKE_VERSION))
$(error GNU Make is required)
endif
make_min_ver := 3.81
ifneq ($(make_min_ver),$(firstword $(sort $(make_min_ver) $(MAKE_VERSION))))
$(error GNU Make $(make_min_ver) or higher is required)
endif

DASH_R		= -r

# Define keyword generator before rules.mk, see bug 323979 comment 50

USE_HOST_CXX = 1

include $(topsrcdir)/config/rules.mk

# Help ensure that the number of OOM errors in SpiderMonkey doesn't increase.
# If the number of OOM errors changes, update the number below. We intend this
# number to go down over time, by fixing OOMs.
check-ooms:
	$(wildcard $(RUN_TEST_PROGRAM)) $(PYTHON) -u $(topsrcdir)/config/find_OOM_errors.py --regression 125

ifeq ($(MOZ_DEBUG),1)
#check:: check-ooms
endif

ifdef MOZ_VALGRIND
ifndef MOZ_ASAN
JITTEST_VALGRIND_FLAG = --valgrind
endif
endif

ifneq ($(LLVM_SYMBOLIZER),)
# Use the LLVM symbolizer when running jit-tests under ASan and TSan, if available
ifdef MOZ_ASAN
JITTEST_SANITIZER_ENV=ASAN_SYMBOLIZER_PATH='$(LLVM_SYMBOLIZER)'
endif
ifdef MOZ_TSAN
JITTEST_SANITIZER_ENV=TSAN_OPTIONS="external_symbolizer_path=$(LLVM_SYMBOLIZER) $$TSAN_OPTIONS"
endif
ifdef MOZ_MSAN
JITTEST_SANITIZER_ENV=MSAN_SYMBOLIZER_PATH='$(LLVM_SYMBOLIZER)'
endif
endif

check-style::
	(cd $(srcdir) && $(PYTHON) $(topsrcdir)/config/check_spidermonkey_style.py);

check-masm::
	(cd $(srcdir) && $(PYTHON) $(topsrcdir)/config/check_macroassembler_style.py);

check-js-msg::
	(cd $(topsrcdir) && $(PYTHON) $(topsrcdir)/config/check_js_msg_encoding.py);

check-opcode::
	(cd $(topsrcdir) && $(PYTHON) $(topsrcdir)/config/check_js_opcode.py);

check-jit-test::
	$(JITTEST_SANITIZER_ENV) $(wildcard $(RUN_TEST_PROGRAM)) $(PYTHON) -u $(srcdir)/jit-test/jit_test.py \
	        --no-slow --no-progress --format=automation --jitflags=all \
			$(JITTEST_VALGRIND_FLAG) \
			$(JITTEST_EXTRA_ARGS) \
	        $(DIST)/bin/$(JS_SHELL_NAME)$(BIN_SUFFIX) $(JITTEST_TEST_ARGS)

check:: check-style check-masm check-js-msg check-opcode

check-jstests:
	$(wildcard $(RUN_TEST_PROGRAM)) $(PYTHON) -u $(srcdir)/tests/jstests.py \
		--no-progress --format=automation --timeout 300 \
		$(JSTESTS_EXTRA_ARGS) \
		$(DIST)/bin/$(JS_SHELL_NAME)$(BIN_SUFFIX)

# FIXME:
# We want to run check-jstests as part of |make check| on all platforms, on
# tinderbox. However, some configurations don't work quite right just yet.
# Rather than risking regressions in major configurations while fixing these
# secondary configuration, we work around them and fix the bugs later.
#
#   Bug 652154: On Windows, SM(!m !t) don't work because of path problems with
#				their unusual directory layout
#
#   Bug 652155: On Mac, SM(d) doesn't work for unknown reasons

ifneq ($(OS_ARCH),WINNT)
ifndef HAVE_DTRACE
#check:: check-jstests
endif
endif

DIST_GARBAGE = config.cache config.log config.status* \
   config/autoconf.mk config/emptyvars.mk \
   js-config.h js-confdefs.h \
   backend.mk config/backend.mk devtools/backend.mk editline/backend.mk \
   gdb/backend.mk jsapi-tests/backend.mk shell/backend.mk tests/backend.mk \
   backend.RecursiveMakeBackend backend.RecursiveMakeBackend.pp \
   devtools/rootAnalysis/Makefile

distclean::
	$(RM) $(DIST_GARBAGE)

CFLAGS += $(MOZ_ZLIB_CFLAGS)

# Silence warnings on AIX/HP-UX from non-GNU compilers
ifndef GNU_CC
ifeq ($(OS_ARCH),AIX)
# Suppress warnings from xlC
# 1540-1281: offsetof() on null non-POD types
# 1540-1608: anonymous unions using static data members
CFLAGS		+= -qsuppress=1540-1281 -qsuppress=1540-1608
CXXFLAGS	+= -qsuppress=1540-1281 -qsuppress=1540-1608
endif
ifeq ($(OS_ARCH),HP-UX)
# Suppress warnings from aCC
# 3055: anonymous unions declaring types
# 4189: offsetof() on non-POD types
CFLAGS		+= +W3055,4189
CXXFLAGS	+= +W3055,4189
endif
endif
ifeq ($(OS_ARCH),SunOS)
ifeq ($(TARGET_CPU),sparc)

ifdef GNU_CC
CFLAGS   += -mcpu=v9
CXXFLAGS += -mcpu=v9
endif # GNU_CC

endif
endif

install::
	$(MAKE) -C build install
	$(MAKE) -C shell install

ifdef HAVE_DTRACE
javascript-trace.h: $(srcdir)/devtools/javascript-trace.d
	dtrace -x nolibs -h -s $(srcdir)/devtools/javascript-trace.d -o javascript-trace.h.in
	sed -e 's/if _DTRACE_VERSION/ifdef INCLUDE_MOZILLA_DTRACE/' \
	    -e '/const/!s/char \*/const char */g' \
	    javascript-trace.h.in > javascript-trace.h

# We can't automatically generate dependencies on auto-generated headers;
# we have to list them explicitly.
$(addsuffix .$(OBJ_SUFFIX),Probes jsinterp jsobj): $(CURDIR)/javascript-trace.h

ifneq ($(OS_ARCH),Darwin)
DTRACE_PROBE_OBJ = js-dtrace.$(OBJ_SUFFIX)
$(LIBRARY): $(DTRACE_PROBE_OBJ)
$(DTRACE_PROBE_OBJ): $(srcdir)/devtools/javascript-trace.d $(OBJS)
	dtrace -x nolibs -G -C -s $< -o $@ $(filter-out $<, $^)

OBJS += $(DTRACE_PROBE_OBJ) # for "make clean"
endif # OS_ARCH != Darwin
endif # HAVE_DTRACE

###############################################
# Generating source package tarballs
# (only possible when tar is found)
ifneq (,$(TAR))

source-package:
	SRCDIR=$(srcdir) \
	DIST=$(DIST) \
	MKDIR=$(MKDIR) \
	TAR=$(TAR) \
	AUTOCONF=$(AUTOCONF) \
	MOZJS_MAJOR_VERSION=$(MOZJS_MAJOR_VERSION) \
	MOZJS_MINOR_VERSION=$(MOZJS_MINOR_VERSION) \
	MOZJS_PATCH_VERSION=$(MOZJS_PATCH_VERSION) \
	MOZJS_ALPHA=$(MOZJS_ALPHA) \
	$(srcdir)/make-source-package.sh

clean::
	DIST=$(DIST) \
	MOZJS_MAJOR_VERSION=$(MOZJS_MAJOR_VERSION) \
	MOZJS_MINOR_VERSION=$(MOZJS_MINOR_VERSION) \
	MOZJS_PATCH_VERSION=$(MOZJS_PATCH_VERSION) \
	MOZJS_ALPHA=$(MOZJS_ALPHA) \
	$(srcdir)/make-source-package.sh clean

endif

include $(topsrcdir)/config/recurse.mk
