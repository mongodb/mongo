AM_CPPFLAGS = -I$(top_builddir) \
    -I$(top_srcdir)/src/include -I$(BERKELEY_DB_PATH)/include

noinst_LTLIBRARIES = libwiredtiger_kvs_bdb.la
libwiredtiger_kvs_bdb_la_SOURCES = kvs_bdb.c
libwiredtiger_kvs_bdb_la_LIBADD = -L$(BERKELEY_DB_PATH)/lib -ldb

# libtool hack: noinst_LTLIBRARIES turns off building shared libraries as well
# as installation, it will only build static libraries.  As far as I can tell,
# the "approved" libtool way to turn them back on is by adding -rpath.
libwiredtiger_kvs_bdb_la_LDFLAGS = -avoid-version -module -rpath /nowhere
