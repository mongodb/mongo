AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

lib_LTLIBRARIES = libwiredtiger_zlib.la
libwiredtiger_zlib_la_SOURCES = zlib_compress.c
if !HAVE_BUILTIN_EXTENSION_ZLIB
libwiredtiger_zlib_la_LDFLAGS = -avoid-version -module
endif
libwiredtiger_zlib_la_LIBADD = -lz
