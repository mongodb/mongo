AM_CPPFLAGS = -I$(top_builddir)
AM_CPPFLAGS +=-I$(top_srcdir)/src/include
AM_CPPFLAGS +=-I$(top_srcdir)/test/utility

noinst_PROGRAMS = intpack-test intpack-test2 intpack-test3 packing-test

LDADD = $(top_builddir)/test/utility/libtest_util.la
LDADD +=$(top_builddir)/libwiredtiger.la
LDFLAGS = -static

TESTS = smoke.sh

clean-local:
	rm -rf core.* *.core
