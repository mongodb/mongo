AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/api/leveldb/include

lib_LTLIBRARIES = libwiredtiger_leveldb.la
LDADD = $(lib_LTLIBRARIES) $(top_builddir)/libwiredtiger.la

noinst_PROGRAMS = leveldb_test

# Setup the LevelDB headers to be installed in a wiredtiger/leveldb
# subdirectory, so we don't interfere with other LevelDB installs.
leveldbincludedir = $(includedir)/wiredtiger/leveldb
leveldbinclude_HEADERS = \
	 include/leveldb/cache.h \
	 include/leveldb/c.h \
	 include/leveldb/comparator.h\
	 include/leveldb/db.h \
	 include/leveldb/env.h \
	 include/leveldb/filter_policy.h \
	 include/leveldb/iterator.h \
	 include/leveldb/options.h \
	 include/leveldb/slice.h \
	 include/leveldb/status.h \
	 include/leveldb/table_builder.h \
	 include/leveldb/table.h \
	 include/leveldb/write_batch.h

if HAVE_ELEVELDB
leveldbinclude_HEADERS += \
	 include/leveldb/perf_count.h
endif

libwiredtiger_leveldb_la_LDFLAGS = -release @VERSION@
libwiredtiger_leveldb_la_SOURCES = \
	leveldb_wt.cc \
	db/write_batch.cc \
	util/coding.cc util/comparator.cc util/env.cc util/env_posix.cc \
	util/logging.cc util/options.cc util/status.cc

if HAVE_ELEVELDB
libwiredtiger_leveldb_la_SOURCES += util/perf_count.cc
endif

leveldb_test_SOURCES = leveldb_test.cc
#leveldb_test_LDADD = $(top_builddir)/libwiredtiger.la

TESTS = $(noinst_PROGRAMS)
