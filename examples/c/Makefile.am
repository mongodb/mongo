LDADD = $(top_builddir)/libwiredtiger.la
AM_CPPFLAGS = -I$(top_srcdir)/src/include

noinst_PROGRAMS = \
	ex_access \
	ex_all \
	ex_async \
	ex_call_center \
	ex_config \
	ex_config_parse \
	ex_cursor \
	ex_data_source \
	ex_extending \
	ex_file \
	ex_hello \
	ex_log \
	ex_pack \
	ex_process \
	ex_schema \
	ex_scope \
	ex_stat \
	ex_thread

# The examples can be run with no arguments as simple smoke tests
TESTS = $(noinst_PROGRAMS)

AM_TESTS_ENVIRONMENT = WIREDTIGER_HOME=`mktemp -d WT_HOME.XXXX` ; export WIREDTIGER_HOME ; rm -rf $$WIREDTIGER_HOME ; mkdir $$WIREDTIGER_HOME ;
# automake 1.11 compatibility
TESTS_ENVIRONMENT = $(AM_TESTS_ENVIRONMENT)

clean-local:
	rm -rf WT_HOME* *.core WiredTiger* *.wt
