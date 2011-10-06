LDADD = $(top_builddir)/libwiredtiger.la

# Most of the examples can be run with no arguments
STANDALONE_EXAMPLES = \
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

noinst_PROGRAMS = $(STANDALONE_EXAMPLES) \
	ex_tpcb

TESTS = $(STANDALONE_EXAMPLES) \
	$(srcdir)/run-tpcb

TESTS_ENVIRONMENT = rm -rf WT_TEST ; mkdir WT_TEST ;
