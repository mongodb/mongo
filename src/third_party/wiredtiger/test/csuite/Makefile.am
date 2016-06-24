AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include \
    -I$(top_srcdir)/test/utility
LDADD = $(top_builddir)/test/utility/libtest_util.la \
    $(top_builddir)/libwiredtiger.la
AM_LDFLAGS = -static

test_wt1965_col_efficiency_SOURCES = wt1965_col_efficiency/main.c
noinst_PROGRAMS = test_wt1965_col_efficiency

test_wt2246_col_append_SOURCES = wt2246_col_append/main.c
noinst_PROGRAMS += test_wt2246_col_append

test_wt2535_insert_race_SOURCES = wt2535_insert_race/main.c
noinst_PROGRAMS += test_wt2535_insert_race

test_wt2447_join_main_table_SOURCES = wt2447_join_main_table/main.c
noinst_PROGRAMS += test_wt2447_join_main_table

test_wt2592_join_schema_SOURCES = wt2592_join_schema/main.c
noinst_PROGRAMS += test_wt2592_join_schema

# Run this during a "make check" smoke test.
TESTS = $(noinst_PROGRAMS)
LOG_COMPILER = $(TEST_WRAPPER)

clean-local:
	rm -rf WT_TEST.* *.core
