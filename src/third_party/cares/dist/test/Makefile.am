ARES_BLD_DIR = $(top_builddir)/..
ARES_SRC_DIR = $(top_srcdir)/..

AUTOMAKE_OPTIONS = foreign subdir-objects nostdinc 1.9.6
ACLOCAL_AMFLAGS = -I ../m4 --install

# Note use of -isystem to force use of local gMock/gTest even if there's an installed version.
CPPFLAGS += -I$(ARES_BLD_DIR)/include \
            -I$(ARES_BLD_DIR)/src/lib \
            -I$(ARES_SRC_DIR)/include \
            -I$(ARES_SRC_DIR)/src/lib \
            -I$(top_builddir)         \
            -isystem $(srcdir)/gmock-1.8.0
CXXFLAGS += -Wall $(PTHREAD_CFLAGS)

# Makefile.inc provides the various *SOURCES and *HEADERS defines
include Makefile.inc

TESTS = arestest fuzzcheck.sh

noinst_LTLIBRARIES = libgmock.la

libgmock_la_SOURCES = \
  gmock-1.8.0/gmock-gtest-all.cc	\
  gmock-1.8.0/gmock/gmock.h		\
  gmock-1.8.0/gtest/gtest.h

libgmock_la_CPPFLAGS = -isystem $(srcdir)/gmock-1.8.0


noinst_PROGRAMS = arestest aresfuzz aresfuzzname dnsdump
EXTRA_DIST = fuzzcheck.sh CMakeLists.txt Makefile.m32 Makefile.msvc README.md buildconf $(srcdir)/fuzzinput/* $(srcdir)/fuzznames/*
arestest_SOURCES = $(TESTSOURCES) $(TESTHEADERS)
arestest_LDADD = libgmock.la $(ARES_BLD_DIR)/src/lib/libcares.la $(PTHREAD_LIBS)

# Not interested in coverage of test code, but linking the test binary needs the coverage option
@CODE_COVERAGE_RULES@
arestest_LDFLAGS = $(CODE_COVERAGE_LDFLAGS)



aresfuzz_SOURCES = $(FUZZSOURCES)
aresfuzz_LDADD = $(ARES_BLD_DIR)/src/lib/libcares.la

aresfuzzname_SOURCES = $(FUZZNAMESOURCES)
aresfuzzname_LDADD = $(ARES_BLD_DIR)/src/lib/libcares.la

dnsdump_SOURCES = $(DUMPSOURCES)
dnsdump_LDADD = $(ARES_BLD_DIR)/src/lib/libcares.la

test: check
