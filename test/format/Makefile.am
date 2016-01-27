AM_CPPFLAGS = -I$(top_builddir) \
    -I$(top_srcdir)/src/include -I$(top_srcdir)/test/utility
if HAVE_BERKELEY_DB
AM_CPPFLAGS += -DHAVE_BERKELEY_DB \
	-DBERKELEY_DB_PATH=\"$(BERKELEY_DB_PATH)\" -I$(BERKELEY_DB_PATH)/include
endif

noinst_PROGRAMS = t
noinst_SCRIPTS = s_dumpcmp
t_SOURCES =\
	config.h format.h backup.c bulk.c compact.c config.c lrt.c ops.c \
	rebalance.c salvage.c t.c util.c wts.c

if HAVE_BERKELEY_DB
t_SOURCES += bdb.c
endif

t_LDADD = $(top_builddir)/libwiredtiger.la
if HAVE_BERKELEY_DB
t_LDADD += -L$(BERKELEY_DB_PATH)/lib -ldb
endif
t_LDFLAGS = -static

#noinst_LTLIBRARIES = lzo_compress.la

# libtool hack: noinst_LTLIBRARIES turns off building shared libraries as well
# as installation, it will only build static libraries.  As far as I can tell,
# the "approved" libtool way to turn them back on is by adding -rpath.
#lzo_compress_la_SOURCES = lzo_compress.c
#lzo_compress_la_LIBADD = -llzo2 -lm
#lzo_compress_la_LDFLAGS = -avoid-version -module -rpath /nowhere

s_dumpcmp: $(srcdir)/s_dumpcmp.sh
	cp $(srcdir)/s_dumpcmp.sh $@
	chmod +x $@

backup:
	rm -rf BACKUP && cp -p -r RUNDIR BACKUP
refresh:
	rm -rf RUNDIR && cp -p -r BACKUP RUNDIR

TESTS = smoke.sh

clean-local:
	rm -rf RUNDIR s_dumpcmp *.core
