LDADD = $(top_builddir)/libwiredtiger.la
AM_CPPFLAGS = -I$(top_srcdir)/src/include

noinst_PROGRAMS = \
	ex_access \
	ex_all \
	ex_call_center \
	ex_config \
	ex_cursor \
	ex_data_source \
	ex_extending \
	ex_file \
	ex_hello \
	ex_pack \
	ex_process \
	ex_schema \
	ex_stat \
	ex_thread

# The examples can be run with no arguments as simple smoke tests
TESTS = $(noinst_PROGRAMS)

TESTS_ENVIRONMENT = WIREDTIGER_HOME=`mktemp -d WT_TEST.XXXX` ; export WIREDTIGER_HOME ; rm -rf $$WIREDTIGER_HOME ; mkdir $$WIREDTIGER_HOME ;

clean-local:
	rm -rf WT_TEST*
