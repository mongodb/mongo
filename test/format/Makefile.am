BDB = $(top_builddir)/db
INCLUDES = -I$(top_builddir) -I$(BDB)

noinst_PROGRAMS = t
noinst_SCRIPTS = s_dumpcmp
t_SOURCES =\
	config.h format.h bdb.c config.c t.c util.c wts.c wts_bulk.c wts_ops.c
t_LDADD = $(top_builddir)/libwiredtiger.la -L$(BDB)/build_unix -ldb
t_LDFLAGS = -static

s_dumpcmp: $(srcdir)/s_dumpcmp.sh
	cp $(srcdir)/s_dumpcmp.sh $@
	chmod +x $@

backup:
	rm -rf BACKUP && cp -p -r RUNDIR BACKUP

clean-local:
	rm -rf RUNDIR *.core
