AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

LDADD = $(top_builddir)/libwiredtiger.la -lm

noinst_PROGRAMS = wtperf
wtperf_LDFLAGS = -static
wtperf_SOURCES =\
	config.c misc.c track.c wtperf.c wtperf_truncate.c wtperf.h wtperf_opt.i

TESTS = smoke.sh
AM_TESTS_ENVIRONMENT = rm -rf WT_TEST ; mkdir WT_TEST ;
# automake 1.11 compatibility
TESTS_ENVIRONMENT = $(AM_TESTS_ENVIRONMENT)

clean-local:
	rm -rf WT_TEST *.core
