AM_CPPFLAGS = -I$(top_builddir)
LDADD = $(top_builddir)/libwiredtiger.la

noinst_PROGRAMS = wttest
wttest_SOURCES = wttest.c

clean-local:
	rm -rf WiredTiger* *.core casket.wt

