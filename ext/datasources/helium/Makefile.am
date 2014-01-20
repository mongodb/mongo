AM_CPPFLAGS = -I$(top_builddir) \
    -I$(top_srcdir)/src/include -I$(HELIUM_PATH)

noinst_LTLIBRARIES = libwiredtiger_helium.la
libwiredtiger_helium_la_SOURCES = helium.c
libwiredtiger_helium_la_LIBADD = -L$(HELIUM_PATH) -lhe

# libtool hack: noinst_LTLIBRARIES turns off building shared libraries as well
# as installation, it will only build static libraries.  As far as I can tell,
# the "approved" libtool way to turn them back on is by adding -rpath.
libwiredtiger_helium_la_LDFLAGS = -avoid-version -module -rpath /nowhere
