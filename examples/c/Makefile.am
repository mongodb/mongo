LDADD = $(top_builddir)/libwiredtiger.la

noinst_PROGRAMS = \
	ex_access \
	ex_all \
	ex_call_center \
	ex_config \
	ex_cursor \
	ex_extending \
	ex_file \
	ex_hello \
	ex_pack \
	ex_process \
	ex_schema \
	ex_stat \
	ex_thread \
	ex_transaction

# The examples can be run with no arguments as simple smoke tests
TESTS = $(noinst_PROGRAMS)

TESTS_ENVIRONMENT = rm -rf WT_TEST ; mkdir WT_TEST ;
