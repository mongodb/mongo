BDB = $(top_builddir)/db
INCLUDES = -I$(top_builddir) -I$(BDB)

noinst_PROGRAMS = t
t_SOURCES = config.h format.h bdb.c config.c t.c util.c wts.c
t_LDADD = $(top_builddir)/libwiredtiger.la -L$(BDB)/build_unix -ldb
t_LDFLAGS = -static

t:
	if ! test -e $(BDB) ; then \
		echo >&2 "Skipping test/format, no 'db' in top-level build" ; \
		exit 0 ; \
	fi

backup:
	rm -rf BACKUP && mkdir BACKUP && cp __* BACKUP/

clean-local:
	rm -rf BACKUP __* vgout.*
