INCLUDES = -I$(top_builddir)

noinst_PROGRAMS = t
t_LDADD = $(top_builddir)/libwiredtiger.la
t_SOURCES = thread.h file.c fops.c rw.c stats.c t.c
t_LDFLAGS = -static

clean-local:
	rm -rf WiredTiger* __* *.core
