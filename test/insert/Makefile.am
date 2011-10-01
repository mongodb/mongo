INCLUDES = -I$(top_builddir)

noinst_PROGRAMS = t
t_SOURCES = insert.c
t_LDADD = $(top_builddir)/libwiredtiger.la
t_LDFLAGS = -static

clean-local:
	rm -rf WiredTiger xx __schema.wt
