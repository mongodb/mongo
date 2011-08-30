INCLUDES = -I$(top_builddir)
LDADD = -L$(top_builddir) -lwiredtiger
VPATH = $(top_builddir)/../$(subdir)

noinst_PROGRAMS = t
t_SOURCES = thread.h t.c load.c run.c stats.c
