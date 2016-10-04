AM_CPPFLAGS = -I$(top_builddir) -I$(srcdir)/leveldb -I$(srcdir)/leveldb/include

lib_LTLIBRARIES = libwiredtiger_leveldb.la

noinst_PROGRAMS = leveldb_test

# Setup the LevelDB headers to be installed in a wiredtiger/leveldb
# subdirectory, so we don't interfere with other LevelDB installs.
if HAVE_HYPERLEVELDB
leveldbincludedir = $(includedir)/wiredtiger/hyperleveldb
else
if HAVE_ROCKSDB
leveldbincludedir = $(includedir)/wiredtiger/rocksdb
else
leveldbincludedir = $(includedir)/wiredtiger/leveldb
endif
endif
leveldbinclude_HEADERS = \
	leveldb_wt_config.h \
	leveldb/include/leveldb/cache.h \
	leveldb/include/leveldb/comparator.h\
	leveldb/include/leveldb/db.h \
	leveldb/include/leveldb/env.h \
	leveldb/include/leveldb/filter_policy.h \
	leveldb/include/leveldb/iterator.h \
	leveldb/include/leveldb/options.h \
	leveldb/include/leveldb/slice.h \
	leveldb/include/leveldb/status.h \
	leveldb/include/leveldb/write_batch.h

if HAVE_BASHOLEVELDB
AM_CPPFLAGS += -I$(srcdir)/leveldb/include/leveldb -I$(srcdir)/basho
leveldbinclude_HEADERS += \
	 basho/perf_count.h
endif
if HAVE_HYPERLEVELDB
AM_CPPFLAGS += -I$(srcdir)/leveldb/include/leveldb -I$(srcdir)/hyperleveldb
leveldbinclude_HEADERS += \
	 hyperleveldb/replay_iterator.h
endif

libwiredtiger_leveldb_la_LDFLAGS = -release @VERSION@
libwiredtiger_leveldb_la_LIBADD = $(top_builddir)/libwiredtiger_static.la
libwiredtiger_leveldb_la_SOURCES = \
	leveldb_wt.cc \
	leveldb/util/coding.cc leveldb/util/comparator.cc leveldb/util/env.cc leveldb/util/env_posix.cc \
	leveldb/util/logging.cc leveldb/util/options.cc leveldb/util/status.cc

if HAVE_BASHOLEVELDB
libwiredtiger_leveldb_la_SOURCES += basho/perf_count.cc
endif
if HAVE_HYPERLEVELDB
libwiredtiger_leveldb_la_SOURCES += hyper_wt.cc
endif
if HAVE_ROCKSDB
libwiredtiger_leveldb_la_SOURCES += rocks_wt.cc rocksdb/write_batch.cc
else
libwiredtiger_leveldb_la_SOURCES += leveldb/db/write_batch.cc
endif

if HAVE_ROCKSDB
pkglib_LTLIBRARIES = librocksdb.la
else
pkglib_LTLIBRARIES = libleveldb.la
endif

libleveldb_la_LDFLAGS = -release @VERSION@
libleveldb_la_LIBADD = $(top_builddir)/libwiredtiger_static.la
libleveldb_la_SOURCES = $(libwiredtiger_leveldb_la_SOURCES)

librocksdb_la_LDFLAGS = -release @VERSION@
librocksdb_la_LIBADD = $(top_builddir)/libwiredtiger_static.la
librocksdb_la_SOURCES = $(libwiredtiger_leveldb_la_SOURCES)

leveldb_test_SOURCES = leveldb_test.cc
leveldb_test_LDADD = libwiredtiger_leveldb.la

TESTS = $(noinst_PROGRAMS)

clean-local:
	rm -rf WTLDB_HOME
