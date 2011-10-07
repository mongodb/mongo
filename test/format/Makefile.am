BDB = $(top_builddir)/db
INCLUDES = -I$(top_builddir) -I$(BDB)

noinst_PROGRAMS = t
noinst_DATA = s_dumpcmp
t_SOURCES = config.h format.h bdb.c config.c t.c util.c wts.c
t_LDADD = $(top_builddir)/libwiredtiger.la -L$(BDB)/build_unix -ldb
t_LDFLAGS = -static

s_dumpcmp: $(srcdir)/s_dumpcmp.in
	cp $(srcdir)/s_dumpcmp.in $@

backup:
	rm -rf BACKUP && mkdir BACKUP && cp __* BACKUP/

clean-local:
	rm -rf BACKUP WiredTiger vgout.* __*
