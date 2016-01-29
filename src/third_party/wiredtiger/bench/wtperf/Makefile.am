AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

LDADD = $(top_builddir)/libwiredtiger.la -lm

noinst_PROGRAMS = wtperf
wtperf_LDFLAGS = -static
wtperf_SOURCES =\
	config.c idle_table_cycle.c misc.c track.c wtperf.c \
	wtperf.h wtperf_opt.i wtperf_throttle.c wtperf_truncate.c

TESTS = smoke.sh
AM_TESTS_ENVIRONMENT = rm -rf WT_TEST ; mkdir WT_TEST ;
# automake 1.11 compatibility
TESTS_ENVIRONMENT = $(AM_TESTS_ENVIRONMENT)

clean-local:
	rm -rf WT_TEST *.core
