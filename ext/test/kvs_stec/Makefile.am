AM_CPPFLAGS = -I$(top_builddir) \
    -I$(top_srcdir)/src/include -I$(STEC_KVS_PATH)/include

noinst_LTLIBRARIES = libwiredtiger_kvs_stec.la
libwiredtiger_kvs_stec_la_SOURCES = kvs_stec.c
libwiredtiger_kvs_stec_la_LIBADD = \
    $(top_builddir)/libwiredtiger.la \
    -L$(STEC_KVS_PATH)/lib -lkvs -L$(BERKELEY_DB_PATH)/lib -ldb

# libtool hack: noinst_LTLIBRARIES turns off building shared libraries as well
# as installation, it will only build static libraries.  As far as I can tell,
# the "approved" libtool way to turn them back on is by adding -rpath.
libwiredtiger_kvs_stec_la_LDFLAGS = -avoid-version -module -rpath /nowhere
