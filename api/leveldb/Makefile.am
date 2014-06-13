AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/api/leveldb/include

lib_LTLIBRARIES = libwiredtiger_leveldb.la
LDADD = $(lib_LTLIBRARIES) $(top_builddir)/libwiredtiger.la

noinst_PROGRAMS = leveldb_test

# Setup the LevelDB headers to be installed in a wiredtiger/leveldb
# subdirectory, so we don't interfere with other LevelDB installs.
leveldbincludedir = $(includedir)/wiredtiger/leveldb
leveldbinclude_HEADERS = \
	 include/cache.h \
	 include/c.h \
	 include/comparator.h\
	 include/db.h \
	 include/env.h \
	 include/filter_policy.h \
	 include/iterator.h \
	 include/options.h \
	 include/slice.h \
	 include/status.h \
	 include/table_builder.h \
	 include/table.h \
	 include/write_batch.h

libwiredtiger_leveldb_la_LDFLAGS = -release @VERSION@
libwiredtiger_leveldb_la_SOURCES = \
	leveldb_wt.cc \
	db/write_batch.cc \
	util/coding.cc util/comparator.cc util/env.cc util/env_posix.cc \
	util/logging.cc util/options.cc util/status.cc

leveldb_test_SOURCES = leveldb_test.cc
#leveldb_test_LDADD = $(top_builddir)/libwiredtiger.la

TESTS = $(noinst_PROGRAMS)
