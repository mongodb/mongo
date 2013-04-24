LDADD = $(top_builddir)/libwiredtiger.la -lm

noinst_PROGRAMS = wtperf
wtperf_LDFLAGS = -static

# The benchmark can be run with no arguments as simple smoke tests
TESTS = $(noinst_PROGRAMS)

TESTS_ENVIRONMENT = rm -rf WT_TEST ; mkdir WT_TEST ;

clean-local:
	rm -rf WT_TEST
