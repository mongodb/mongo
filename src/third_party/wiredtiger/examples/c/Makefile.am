LDADD = $(top_builddir)/libwiredtiger.la
AM_CPPFLAGS = -I$(top_srcdir)/src/include

noinst_PROGRAMS = \
	ex_access \
	ex_all \
	ex_async \
	ex_backup \
	ex_call_center \
	ex_config_parse \
	ex_cursor \
	ex_data_source \
	ex_encrypt \
	ex_event_handler \
	ex_extending \
	ex_extractor \
	ex_file_system \
	ex_hello \
	ex_log \
	ex_pack \
	ex_process \
	ex_schema \
	ex_scope \
	ex_stat \
	ex_sync \
	ex_thread

ex_encrypt_LDFLAGS = -rdynamic
ex_file_system_LDFLAGS = -rdynamic

# The examples can be run with no arguments as simple smoke tests
TESTS = $(noinst_PROGRAMS)

AM_TESTS_ENVIRONMENT = WIREDTIGER_HOME=`mktemp -d WT_HOME.XXXX` ; export WIREDTIGER_HOME ; rm -rf $$WIREDTIGER_HOME ; mkdir $$WIREDTIGER_HOME ;

# Inject valgrind or similar
LOG_COMPILER = $(TEST_WRAPPER)

clean-local:
	rm -rf WT_HOME* *.core backup_full.* backup_incr.*
