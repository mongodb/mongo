BDB = $(top_builddir)/db
INCLUDES = -I$(top_builddir) -I$(BDB)
LDADD = -L$(top_builddir) -lwiredtiger -L$(BDB)/build_unix -ldb
VPATH = $(top_builddir)/../$(subdir)

noinst_PROGRAMS = t
t_SOURCES = insert.c
