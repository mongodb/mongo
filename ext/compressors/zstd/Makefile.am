AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

if HAVE_BUILTIN_EXTENSION_ZSTD
noinst_LTLIBRARIES = libwiredtiger_zstd.la
else
lib_LTLIBRARIES = libwiredtiger_zstd.la
libwiredtiger_zstd_la_LDFLAGS = -avoid-version -module
endif

libwiredtiger_zstd_la_SOURCES = zstd_compress.c
libwiredtiger_zstd_la_LIBADD = -lzstd
