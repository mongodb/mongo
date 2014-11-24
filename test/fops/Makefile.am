AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

noinst_PROGRAMS = t
t_LDADD = $(top_builddir)/libwiredtiger.la
t_SOURCES = thread.h file.c fops.c t.c
t_LDFLAGS = -static

TESTS = $(noinst_PROGRAMS)

clean-local:
	rm -rf WT_TEST
