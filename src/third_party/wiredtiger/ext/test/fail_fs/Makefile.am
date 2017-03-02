AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

noinst_LTLIBRARIES = libwiredtiger_fail_fs.la
libwiredtiger_fail_fs_la_SOURCES = fail_fs.c

# libtool hack: noinst_LTLIBRARIES turns off building shared libraries as well
# as installation, it will only build static libraries.  As far as I can tell,
# the "approved" libtool way to turn them back on is by adding -rpath.
libwiredtiger_fail_fs_la_LDFLAGS = -avoid-version -module -rpath /nowhere
