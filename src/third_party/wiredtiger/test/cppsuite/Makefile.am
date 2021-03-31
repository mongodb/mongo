AM_CPPFLAGS = -std='c++11' -I$(top_builddir) -I$(top_srcdir)/src/include \
    -I$(top_srcdir)/test/utility -I$(top_srcdir)/test/cppsuite
LDADD = $(top_builddir)/test/utility/libtest_util.la \
    $(top_builddir)/libwiredtiger.la
AM_LDFLAGS = -static

# Copy across the default configuration files with the artifacts.
all:
	rsync -rup $(top_srcdir)/test/cppsuite/configs $(top_builddir)/test/cppsuite

all_TESTS=
noinst_PROGRAMS=

run_SOURCES = tests/run.cxx
noinst_PROGRAMS += run
all_TESTS += run

# Run this during a "make check" smoke test.
TESTS = $(all_TESTS)

clean-local:
	rm -rf WT_TEST.* core.* *.core
