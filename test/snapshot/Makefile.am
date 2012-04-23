INCLUDES = -I$(top_builddir) -I$(top_srcdir)/src/include

noinst_PROGRAMS = t
t_SOURCES = snapshot.c
t_LDADD = $(top_builddir)/libwiredtiger.la
t_LDFLAGS = -static

clean-local:
	rm -rf WiredTiger WiredTiger.* __* *.core
