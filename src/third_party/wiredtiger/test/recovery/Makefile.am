AM_CPPFLAGS = -I$(top_builddir)
AM_CPPFLAGS +=-I$(top_srcdir)/src/include
AM_CPPFLAGS +=-I$(top_srcdir)/test/utility

noinst_PROGRAMS = random-abort timestamp-abort truncated-log
random_abort_SOURCES = random-abort.c
random_abort_LDADD = $(top_builddir)/test/utility/libtest_util.la
random_abort_LDADD +=$(top_builddir)/libwiredtiger.la
random_abort_LDFLAGS = -static

timestamp_abort_SOURCES = timestamp-abort.c
timestamp_abort_LDADD = $(top_builddir)/test/utility/libtest_util.la
timestamp_abort_LDADD +=$(top_builddir)/libwiredtiger.la
timestamp_abort_LDFLAGS = -static

truncated_log_SOURCES = truncated-log.c
truncated_log_LDADD = $(top_builddir)/test/utility/libtest_util.la
truncated_log_LDADD +=$(top_builddir)/libwiredtiger.la
truncated_log_LDFLAGS = -static

# Run this during a "make check" smoke test.
TESTS = smoke.sh

clean-local:
	rm -rf WT_TEST.* *.core
