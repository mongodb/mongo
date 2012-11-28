BDB = $(top_builddir)/db
AM_CPPFLAGS = -DWIREDTIGER_TEST_COMPRESS_RAW \
	-I$(top_builddir) -I$(top_srcdir)/src/include -I$(BDB)

noinst_PROGRAMS = t
noinst_SCRIPTS = s_dumpcmp
t_SOURCES =\
	config.h format.h bdb.c config.c t.c util.c wts.c wts_bulk.c wts_ops.c
t_LDADD = $(top_builddir)/libwiredtiger.la -L$(BDB)/build_unix -ldb
t_LDFLAGS = -static

noinst_LTLIBRARIES = raw_compress.la
raw_compress_la_SOURCES = \
	$(top_srcdir)/ext/compressors/bzip2/bzip2_compress.c
raw_compress_la_LIBADD = -lbz2

# libtool hack: noinst_LTLIBRARIES turns off building shared libraries as well
# as installation, it will only build static libraries.  As far as I can tell,
# the "approved" libtool way to turn them back on is by adding -rpath.
raw_compress_la_LDFLAGS = -avoid-version -module -rpath /nowhere

s_dumpcmp: $(srcdir)/s_dumpcmp.sh
	cp $(srcdir)/s_dumpcmp.sh $@
	chmod +x $@

backup:
	rm -rf BACKUP && cp -p -r RUNDIR BACKUP

clean-local:
	rm -rf RUNDIR *.core
