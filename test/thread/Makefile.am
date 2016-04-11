AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include \
    -I$(top_srcdir)/test/utility

noinst_PROGRAMS = t
t_LDADD = $(top_builddir)/libwiredtiger.la
t_SOURCES = thread.h file.c rw.c stats.c t.c
t_LDFLAGS = -static

TESTS = smoke.sh

clean-local:
	rm -rf WT_TEST __stats *.core
