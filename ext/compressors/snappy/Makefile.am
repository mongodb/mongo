AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

lib_LTLIBRARIES = libwiredtiger_snappy.la
libwiredtiger_snappy_la_SOURCES = snappy_compress.c
libwiredtiger_snappy_la_LDFLAGS = -avoid-version -module
libwiredtiger_snappy_la_LIBADD = -lsnappy
