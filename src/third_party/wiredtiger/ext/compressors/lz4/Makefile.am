AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

if HAVE_BUILTIN_EXTENSION_LZ4
noinst_LTLIBRARIES = libwiredtiger_lz4.la
else
lib_LTLIBRARIES = libwiredtiger_lz4.la
libwiredtiger_lz4_la_LDFLAGS = -avoid-version -module
endif

libwiredtiger_lz4_la_SOURCES = lz4_compress.c
libwiredtiger_lz4_la_LIBADD = -llz4
