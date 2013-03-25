AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

lib_LTLIBRARIES = libwiredtiger_bzip2.la
libwiredtiger_bzip2_la_SOURCES = bzip2_compress.c
libwiredtiger_bzip2_la_LDFLAGS = -avoid-version -module
libwiredtiger_bzip2_la_LIBADD = -lbz2
