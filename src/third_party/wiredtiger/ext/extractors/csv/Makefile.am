AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

noinst_LTLIBRARIES = libwiredtiger_csv_extractor.la
libwiredtiger_csv_extractor_la_SOURCES = csv_extractor.c

# libtool hack: noinst_LTLIBRARIES turns off building shared libraries as well
# as installation, it will only build static libraries.  As far as I can tell,
# the "approved" libtool way to turn them back on is by adding -rpath.
libwiredtiger_csv_extractor_la_LDFLAGS = \
	-avoid-version -module -rpath /nowhere
