AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

noinst_LTLIBRARIES = libwiredtiger_revint_collator.la
libwiredtiger_revint_collator_la_SOURCES = revint_collator.c

# libtool hack: noinst_LTLIBRARIES turns off building shared libraries as well
# as installation, it will only build static libraries.  As far as I can tell,
# the "approved" libtool way to turn them back on is by adding -rpath.
libwiredtiger_revint_collator_la_LDFLAGS = \
	-avoid-version -module -rpath /nowhere
