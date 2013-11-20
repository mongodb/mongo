AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include

LDADD = $(top_builddir)/libwiredtiger.la -lm

noinst_PROGRAMS = wtperf
wtperf_LDFLAGS = -static
wtperf_SOURCES =\
	config.c misc.c wtperf.c wtperf.h wtperf_opt.i

# The benchmark can be run with no arguments as simple smoke tests
TESTS = $(noinst_PROGRAMS)

TESTS_ENVIRONMENT = rm -rf WT_TEST ; mkdir WT_TEST ;

clean-local:
	rm -rf WT_TEST *.core
