AM_CPPFLAGS = -I$(top_builddir)
AM_CPPFLAGS +=-I$(top_srcdir)/src/include
AM_CPPFLAGS +=-I$(top_srcdir)/test/utility

noinst_PROGRAMS = wtperf
wtperf_SOURCES =\
	config.c idle_table_cycle.c misc.c track.c wtperf.c \
	wtperf.h wtperf_opt.i wtperf_throttle.c wtperf_truncate.c

wtperf_LDADD = $(top_builddir)/test/utility/libtest_util.la
wtperf_LDADD +=$(top_builddir)/libwiredtiger.la
wtperf_LDADD +=-lm
wtperf_LDFLAGS = -static

TESTS = smoke.sh
AM_TESTS_ENVIRONMENT = rm -rf WT_TEST ; mkdir WT_TEST ;
# automake 1.11 compatibility
TESTS_ENVIRONMENT = $(AM_TESTS_ENVIRONMENT)

clean-local:
	rm -rf WT_TEST core.* *.core
