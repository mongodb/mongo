AM_CPPFLAGS = -I$(top_builddir)
AM_CPPFLAGS +=-I$(top_srcdir)/src/include
AM_CPPFLAGS +=-I$(top_srcdir)/test/utility

noinst_PROGRAMS = t
t_SOURCES =\
	alter.c backup.c bulk.c checkpoint.c compact.c config.c config_compat.c hs.c import.c kv.c ops.c \
	random.c salvage.c snap.c t.c trace.c util.c wts.c

t_LDADD = $(top_builddir)/test/utility/libtest_util.la
t_LDADD +=$(top_builddir)/libwiredtiger.la
t_LDFLAGS = -static

#noinst_LTLIBRARIES = lzo_compress.la

# libtool hack: noinst_LTLIBRARIES turns off building shared libraries as well
# as installation, it will only build static libraries.  As far as I can tell,
# the "approved" libtool way to turn them back on is by adding -rpath.
#lzo_compress_la_SOURCES = lzo_compress.c
#lzo_compress_la_LIBADD = -llzo2 -lm
#lzo_compress_la_LDFLAGS = -avoid-version -module -rpath /nowhere

backup:
	rm -rf BACKUP && cp -p -r RUNDIR BACKUP
refresh:
	rm -rf RUNDIR && cp -p -r BACKUP RUNDIR

TESTS = smoke.sh

clean-local:
	rm -rf RUNDIR s_dumpcmp core.* *.core
