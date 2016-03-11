AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include \
    -I$(top_srcdir)/test/utility

noinst_PROGRAMS = t
t_SOURCES = readonly.c
t_LDADD = $(top_builddir)/libwiredtiger.la
t_LDFLAGS = -static

# Run this during a "make check" smoke test.
TESTS = smoke.sh

clean-local:
	rm -rf WT_RD* WiredTiger* *.core __*
