AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

if HAVE_BUILTIN_EXTENSION_SODIUM
noinst_LTLIBRARIES = libwiredtiger_sodium.la
else
lib_LTLIBRARIES = libwiredtiger_sodium.la
libwiredtiger_sodium_la_LDFLAGS = -avoid-version -module
endif
libwiredtiger_sodium_la_SOURCES = sodium_encrypt.c
libwiredtiger_sodium_la_LIBADD = -lsodium
