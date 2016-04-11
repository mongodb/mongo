AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include \
    -I$(top_srcdir)/test/utility

noinst_PROGRAMS = cursor_order
cursor_order_LDADD = $(top_builddir)/libwiredtiger.la

cursor_order_SOURCES = cursor_order_file.c cursor_order_ops.c cursor_order.c
cursor_order_LDFLAGS = -static

TESTS = $(noinst_PROGRAMS)

clean-local:
	rm -rf WT_TEST *.core
