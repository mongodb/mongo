AM_CPPFLAGS = -I$(top_builddir)

noinst_PROGRAMS = t
t_LDADD = $(top_builddir)/libwiredtiger.la
t_SOURCES = thread.h file.c rw.c stats.c t.c
t_LDFLAGS = -static

TESTS = $(noinst_PROGRAMS)

clean-local:
	rm -rf WiredTiger* __* *.core
