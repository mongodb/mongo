AM_CPPFLAGS = -I$(top_builddir)
AM_CPPFLAGS +=-I$(top_srcdir)/src/include
AM_CPPFLAGS +=-I$(top_srcdir)/test/utility

noinst_PROGRAMS = t
t_SOURCES = huge.c

t_LDADD = $(top_builddir)/test/utility/libtest_util.la
t_LDADD +=$(top_builddir)/libwiredtiger.la
t_LDFLAGS = -static

# Run this during a "make check" smoke test.
TESTS = smoke.sh

clean-local:
	rm -rf WT_TEST *.core
