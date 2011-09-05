BDB = $(top_builddir)/db
INCLUDES = -I$(top_builddir) -I$(BDB)
LDADD = -L$(top_builddir) -lwiredtiger -L$(BDB)/build_unix -ldb

noinst_PROGRAMS = t
t_SOURCES = config.h format.h bdb.c config.c t.c util.c wts.c

backup:
	rm -rf BACKUP && mkdir BACKUP && cp __* BACKUP/

clean-local:
	rm -rf BACKUP __*
