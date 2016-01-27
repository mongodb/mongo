AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

noinst_PROGRAMS = intpack-test intpack-test2 intpack-test3 packing-test
LDADD = $(top_builddir)/libwiredtiger.la
LDFLAGS = -static

TESTS = smoke.sh

clean-local:
	rm -rf *.core
