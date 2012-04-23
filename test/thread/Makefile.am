INCLUDES = -I$(top_builddir)

noinst_PROGRAMS = t
t_LDADD = $(top_builddir)/libwiredtiger.la
t_SOURCES = thread.h t.c load.c run.c stats.c
t_LDFLAGS = -static

clean-local:
	rm -rf WiredTiger* __* *.core
