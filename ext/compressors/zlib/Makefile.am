AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

lib_LTLIBRARIES = libwiredtiger_zlib.la
libwiredtiger_zlib_la_SOURCES = zlib_compress.c
libwiredtiger_zlib_la_LDFLAGS = -avoid-version -module
libwiredtiger_zlib_la_LIBADD = -lz
