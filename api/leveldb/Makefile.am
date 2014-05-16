AM_CPPFLAGS = -I$(top_builddir)

lib_LTLIBRARIES = libwiredtiger_leveldb.la
LDADD = $(lib_LTLIBRARIES) $(top_builddir)/libwiredtiger.la

noinst_PROGRAMS = leveldb_test

libwiredtiger_leveldb_la_LDFLAGS = -release @VERSION@
libwiredtiger_leveldb_la_SOURCES = \
	leveldb_wt.cc \
	db/write_batch.cc \
	util/coding.cc util/comparator.cc util/env.cc util/env_posix.cc \
	util/logging.cc util/options.cc util/status.cc

leveldb_test_SOURCES = leveldb_test.cc
#leveldb_test_LDADD = $(top_builddir)/libwiredtiger.la

TESTS = $(noinst_PROGRAMS)
