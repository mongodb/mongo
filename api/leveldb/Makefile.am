AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/api/leveldb/include

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
	wiredtiger_config.h \
	include/leveldb/cache.h \
	include/leveldb/comparator.h\
	include/leveldb/db.h \
	include/leveldb/env.h \
	include/leveldb/filter_policy.h \
	include/leveldb/iterator.h \
	include/leveldb/options.h \
	include/leveldb/slice.h \
	include/leveldb/status.h \
	include/leveldb/write_batch.h

if HAVE_ELEVELDB
leveldbinclude_HEADERS += \
	 include/leveldb/perf_count.h
endif
if HAVE_HYPERLEVELDB
leveldbinclude_HEADERS += \
	 include/leveldb/replay_iterator.h
endif

libwiredtiger_leveldb_la_LDFLAGS = -release @VERSION@
libwiredtiger_leveldb_la_LIBADD = $(top_builddir)/libwiredtiger.la
libwiredtiger_leveldb_la_SOURCES = \
	leveldb_wt.cc \
	db/write_batch.cc \
	util/coding.cc util/comparator.cc util/env.cc util/env_posix.cc \
	util/logging.cc util/options.cc util/status.cc

if HAVE_ELEVELDB
libwiredtiger_leveldb_la_SOURCES += util/perf_count.cc
endif
if HAVE_ROCKSDB
libwiredtiger_leveldb_la_SOURCES += rocks_wt.cc
endif

if HAVE_ROCKSDB
pkglib_LTLIBRARIES = librocksdb.la
else
pkglib_LTLIBRARIES = libleveldb.la
endif

libleveldb_la_LDFLAGS = -release @VERSION@
libleveldb_la_LIBADD = libwiredtiger_leveldb.la
libleveldb_la_SOURCES = dummy.cc

librocksdb_la_LDFLAGS = -release @VERSION@
librocksdb_la_LIBADD = libwiredtiger_leveldb.la
librocksdb_la_SOURCES = dummy.cc

leveldb_test_SOURCES = leveldb_test.cc
leveldb_test_LDADD = libwiredtiger_leveldb.la

TESTS = $(noinst_PROGRAMS)
