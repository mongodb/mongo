AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include \
    -I$(top_srcdir)/test/utility

noinst_PROGRAMS = t
t_LDADD = $(top_builddir)/libwiredtiger.la
t_SOURCES = checkpointer.c workers.c test_checkpoint.c
t_LDFLAGS = -static

TESTS = smoke.sh

clean-local:
	rm -rf WT_TEST *.core
