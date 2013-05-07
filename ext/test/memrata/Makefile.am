AM_CPPFLAGS = -I$(top_builddir) \
    -I$(top_srcdir)/src/include -I$(MEMRATA_PATH)

noinst_LTLIBRARIES = libwiredtiger_memrata.la
libwiredtiger_memrata_la_SOURCES = memrata.c
libwiredtiger_memrata_la_LIBADD = \
    -L$(MEMRATA_PATH) -lkvs -L$(BERKELEY_DB_PATH)/lib -ldb

# libtool hack: noinst_LTLIBRARIES turns off building shared libraries as well
# as installation, it will only build static libraries.  As far as I can tell,
# the "approved" libtool way to turn them back on is by adding -rpath.
libwiredtiger_memrata_la_LDFLAGS = -avoid-version -module -rpath /nowhere
