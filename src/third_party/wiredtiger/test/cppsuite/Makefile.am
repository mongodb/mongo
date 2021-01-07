AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include \
    -I$(top_srcdir)/test/utility -I$(top_srcdir)/test/cppsuite
LDADD = $(top_builddir)/test/utility/libtest_util.la \
    $(top_builddir)/libwiredtiger.la
AM_LDFLAGS = -static

all_TESTS=
noinst_PROGRAMS=

test_poc_SOURCES = tests/poc.cxx
noinst_PROGRAMS += test_poc
all_TESTS += test_poc

# Run this during a "make check" smoke test.
TESTS = $(all_TESTS)

clean-local:
	rm -rf WT_TEST.* core.* *.core
