## Process this file with automake to produce Makefile.in

# Make sure that when we re-make ./configure, we get the macros we need
ACLOCAL_AMFLAGS = -I m4

# This is so we can #include <gflags/foo>
AM_CPPFLAGS = -I$(top_srcdir)/src

# This is mostly based on configure options
AM_CXXFLAGS =

# These are good warnings to turn on by default,
if GCC
AM_CXXFLAGS += -Wall -Wwrite-strings -Woverloaded-virtual -Wno-sign-compare
endif

# The -no-undefined flag allows libtool to generate shared libraries for
# Cygwin and MinGW.  LIBSTDCXX_LA_LINKER_FLAG is used to fix a Solaris bug.
AM_LDFLAGS = -no-undefined $(LIBSTDCXX_LA_LINKER_FLAG)

gflagsincludedir = $(includedir)/gflags
## The .h files you want to install (that is, .h files that people
## who install this package can include in their own applications.)
gflagsinclude_HEADERS = src/gflags/gflags.h src/gflags/gflags_declare.h \
                        src/gflags/gflags_completions.h

# This is for backwards compatibility only.
googleincludedir = $(includedir)/google
googleinclude_HEADERS = src/google/gflags.h src/google/gflags_completions.h

bin_SCRIPTS = src/gflags_completions.sh

docdir = $(prefix)/share/doc/$(PACKAGE)-$(VERSION)
## This is for HTML and other documentation you want to install.
## Add your documentation files (in doc/) in addition to these
## top-level boilerplate files.  Also add a TODO file if you have one.
dist_doc_DATA = AUTHORS COPYING ChangeLog INSTALL NEWS README \
	README_windows.txt doc/designstyle.css doc/gflags.html

## The libraries (.so's) you want to install
lib_LTLIBRARIES =
## The location of the windows project file for each binary we make
WINDOWS_PROJECTS = gflags.sln

## unittests you want to run when people type 'make check'.
## TESTS is for binary unittests, check_SCRIPTS for script-based unittests.
## TESTS_ENVIRONMENT sets environment variables for when you run unittest,
## but it only seems to take effect for *binary* unittests (argh!)
TESTS =
TESTS_ENVIRONMENT = SRCDIR="$(top_srcdir)"
check_SCRIPTS =
# Every time you add a unittest to check_SCRIPTS, add it here too
noinst_SCRIPTS =
# Used for auto-generated source files
CLEANFILES =

## vvvv RULES TO MAKE THE LIBRARIES, BINARIES, AND UNITTESTS

GFLAGS_SOURCES = $(gflagsinclude_HEADERS) src/mutex.h src/util.h \
                 src/gflags.cc src/gflags_reporting.cc \
                 src/gflags_completions.cc

lib_LTLIBRARIES += libgflags.la
WINDOWS_PROJECTS += vsprojects/libgflags/libgflags.vcproj
libgflags_la_SOURCES = $(GFLAGS_SOURCES)
libgflags_la_CXXFLAGS = $(PTHREAD_CFLAGS) -DNDEBUG
# -version-info gets passed to libtool
libgflags_la_LDFLAGS = $(PTHREAD_CFLAGS) -version-info @SO_VERSION@
libgflags_la_LIBADD = $(PTHREAD_LIBS)

lib_LTLIBRARIES += libgflags_nothreads.la
libgflags_nothreads_la_SOURCES = $(GFLAGS_SOURCES)
libgflags_nothreads_la_CXXFLAGS = -DNDEBUG -DNO_THREADS
libgflags_nothreads_la_LDFLAGS = -version-info @SO_VERSION@

TESTS += gflags_unittest
WINDOWS_PROJECTS += vsprojects/gflags_unittest/gflags_unittest.vcproj
gflags_unittest_SOURCES = $(gflagsinclude_HEADERS) \
                          src/config_for_unittests.h \
                          src/gflags_unittest.cc
gflags_unittest_CXXFLAGS = $(PTHREAD_CFLAGS)
gflags_unittest_LDFLAGS = $(PTHREAD_CFLAGS)
gflags_unittest_LDADD = libgflags.la

# Also make sure this works when we don't link in pthreads
TESTS += gflags_nothreads_unittest
gflags_nothreads_unittest_SOURCES = $(gflags_unittest_SOURCES)
gflags_nothreads_unittest_LDADD = libgflags_nothreads.la

# We also want to test that things work properly when the file that
# holds main() has a name ending with -main or _main.  To keep the
# Makefile small :-), we test the no-threads version of these.
TESTS += gflags_unittest2
gflags_unittest2_SOURCES = $(gflagsinclude_HEADERS) \
                           src/gflags_unittest-main.cc
gflags_unittest2_LDADD = libgflags_nothreads.la
src/gflags_unittest-main.cc: src/gflags_unittest.cc
	rm -f src/gflags_unittest-main.cc
	cp -p $(top_srcdir)/src/gflags_unittest.cc src/gflags_unittest-main.cc
CLEANFILES += src/gflags_unittest-main.cc

TESTS += gflags_unittest3
gflags_unittest3_SOURCES = $(gflagsinclude_HEADERS) \
	                   src/gflags_unittest_main.cc
gflags_unittest3_LDADD = libgflags_nothreads.la
src/gflags_unittest_main.cc: src/gflags_unittest.cc
	rm -f src/gflags_unittest_main.cc
	cp -p $(top_srcdir)/src/gflags_unittest.cc src/gflags_unittest_main.cc
CLEANFILES += src/gflags_unittest_main.cc

# Some buggy sh's ignore "" instead of treating it as a positional
# parameter.  Since we use "" in this script, we prefer bash if we
# can.  If there's no bash, we fall back to sh.
check_SCRIPTS += gflags_unittest_sh
noinst_SCRIPTS += src/gflags_unittest.sh
dist_noinst_DATA = src/gflags_unittest_flagfile
gflags_unittest_sh: gflags_unittest$(EXEEXT) \
                    gflags_unittest2$(EXEEXT) \
                    gflags_unittest3$(EXEEXT)
	bash --version >/dev/null 2>&1 && export SH=bash || export SH=sh; \
	$$SH "$(top_srcdir)/src/gflags_unittest.sh" \
	     "`pwd`/gflags_unittest" "$(top_srcdir)" "@TEST_TMPDIR@"

# Test the STRIP_FLAGS #define.
TESTS += gflags_strip_flags_test
gflags_strip_flags_test_SOURCES = $(gflagsinclude_HEADERS) \
                                  src/config_for_unittests.h \
                                  src/gflags_strip_flags_test.cc
gflags_strip_flags_test_CXXFLAGS = $(PTHREAD_CFLAGS)
gflags_strip_flags_test_LDFLAGS = $(PTHREAD_CFLAGS)
gflags_strip_flags_test_LDADD = libgflags.la

check_SCRIPTS += gflags_strip_flags_test_sh
noinst_SCRIPTS += src/gflags_strip_flags_test.sh
gflags_strip_flags_test_sh: gflags_strip_flags_test$(EXEEXT)
	sh "$(top_srcdir)/src/gflags_strip_flags_test.sh" \
           "`pwd`/gflags_strip_flags_test$(EXEEXT)"

# These are negative-compilation tests.  We want to make sure these
# erroneous use of the flags macros correctly fail to compile.
# Again, we just bother testing with the no-threads version of the library.
check_SCRIPTS += gflags_nc_test1
gflags_nc_test1: $(gflagsinclude_HEADERS) src/gflags_nc.cc
	if $(CXX) -DTEST_SWAPPED_ARGS $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS) -c -o gflags_nc_test1.o $(srcdir)/src/gflags_nc.cc; then echo "Compile succeeded but should have failed"; exit 1; else echo "Compile failed, like it was supposed to"; fi

check_SCRIPTS += gflags_nc_test2
gflags_nc_test2: $(gflagsinclude_HEADERS) src/gflags_nc.cc
	if $(CXX) -DTEST_INT_INSTEAD_OF_BOOL $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS) -c -o gflags_nc_test2.o $(srcdir)/src/gflags_nc.cc; then echo "Compile succeeded but should have failed"; exit 1; else echo "Compile failed, like it was supposed to"; fi

check_SCRIPTS += gflags_nc_test3
gflags_nc_test3: $(gflagsinclude_HEADERS) src/gflags_nc.cc
	if $(CXX) -DTEST_BOOL_IN_QUOTES $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS) -c -o gflags_nc_test3.o $(srcdir)/src/gflags_nc.cc; then echo "Compile succeeded but should have failed"; exit 1; else echo "Compile failed, like it was supposed to"; fi

# This one, on the other hand, should succeed.
check_SCRIPTS += gflags_nc_test4
gflags_nc_test4: $(gflagsinclude_HEADERS) src/gflags_nc.cc
	$(CXX) -DSANITY $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS) -c -o gflags_nc_test4.o $(srcdir)/src/gflags_nc.cc

# This file isn't covered under any rule that would cause it to be distributed.
dist_noinst_DATA += src/gflags_nc.cc


## ^^^^ END OF RULES TO MAKE THE LIBRARIES, BINARIES, AND UNITTESTS


## This should always include $(TESTS), but may also include other
## binaries that you compile but don't want automatically installed.
noinst_PROGRAMS = $(TESTS)

rpm: dist-gzip packages/rpm.sh packages/rpm/rpm.spec
	@cd packages && ./rpm.sh ${PACKAGE} ${VERSION}

deb: dist-gzip packages/deb.sh packages/deb/*
	@cd packages && ./deb.sh ${PACKAGE} ${VERSION}

# http://linux.die.net/man/1/pkg-config, http://pkg-config.freedesktop.org/wiki
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = lib${PACKAGE}.pc lib${PACKAGE}_nothreads.pc
CLEANFILES += $(pkgconfig_DATA)

# I get the description and URL lines from the rpm spec. I use sed to
# try to rewrite exec_prefix, libdir, and includedir in terms of
# prefix, if possible.
lib${PACKAGE}.pc: Makefile packages/rpm/rpm.spec
	echo 'prefix=$(prefix)' > "$@".tmp
	echo 'exec_prefix='`echo '$(exec_prefix)' | sed 's@^$(prefix)@$${prefix}@'` >> "$@".tmp
	echo 'libdir='`echo '$(libdir)' | sed 's@^$(exec_prefix)@$${exec_prefix}@'` >> "$@".tmp
	echo 'includedir='`echo '$(includedir)' | sed 's@^$(prefix)@$${prefix}@'` >> "$@".tmp
	echo '' >> "$@".tmp
	echo 'Name: $(PACKAGE)' >> "$@".tmp
	echo 'Version: $(VERSION)' >> "$@".tmp
	-grep '^Summary:' $(top_srcdir)/packages/rpm/rpm.spec | sed s/^Summary:/Description:/ | head -n1 >> "$@".tmp
	-grep '^URL: ' $(top_srcdir)/packages/rpm/rpm.spec >> "$@".tmp
	echo 'Requires:' >> "$@".tmp
	echo 'Libs: -L$${libdir} -l$(PACKAGE)' >> "$@".tmp
	echo 'Libs.private: $(PTHREAD_CFLAGS) $(PTHREAD_LIBS)' >> "$@".tmp
	echo 'Cflags: -I$${includedir}' >> "$@".tmp
	mv -f "$@".tmp "$@"

# The nothreads version is mostly the same
lib${PACKAGE}_nothreads.pc: lib${PACKAGE}.pc
	grep -v Libs.private lib${PACKAGE}.pc | sed s/-l$(PACKAGE)/-l$(PACKAGE)_nothreads/ > "$@"

libtool: $(LIBTOOL_DEPS)
	$(SHELL) ./config.status --recheck

EXTRA_DIST = packages/rpm.sh packages/rpm/rpm.spec packages/deb.sh packages/deb \
	libtool $(SCRIPTS) \
	src/windows/config.h src/windows/port.h src/windows/port.cc \
	src/windows/gflags/gflags.h src/windows/gflags/gflags_declare.h \
        src/windows/gflags/gflags_completions.h \
	$(WINDOWS_PROJECTS) \
        src/solaris/libstdc++.la
