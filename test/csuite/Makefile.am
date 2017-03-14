AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include \
    -I$(top_srcdir)/test/utility
LDADD = $(top_builddir)/test/utility/libtest_util.la \
    $(top_builddir)/libwiredtiger.la
AM_LDFLAGS = -static

noinst_PROGRAMS=

test_scope_SOURCES = scope/main.c
noinst_PROGRAMS += test_scope

test_wt1965_col_efficiency_SOURCES = wt1965_col_efficiency/main.c
noinst_PROGRAMS += test_wt1965_col_efficiency

test_wt2403_lsm_workload_SOURCES = wt2403_lsm_workload/main.c
noinst_PROGRAMS += test_wt2403_lsm_workload

test_wt2246_col_append_SOURCES = wt2246_col_append/main.c
noinst_PROGRAMS += test_wt2246_col_append

test_wt2323_join_visibility_SOURCES = wt2323_join_visibility/main.c
noinst_PROGRAMS += test_wt2323_join_visibility

test_wt2535_insert_race_SOURCES = wt2535_insert_race/main.c
noinst_PROGRAMS += test_wt2535_insert_race

test_wt2447_join_main_table_SOURCES = wt2447_join_main_table/main.c
noinst_PROGRAMS += test_wt2447_join_main_table

test_wt2695_checksum_SOURCES = wt2695_checksum/main.c
noinst_PROGRAMS += test_wt2695_checksum

test_wt2592_join_schema_SOURCES = wt2592_join_schema/main.c
noinst_PROGRAMS += test_wt2592_join_schema

test_wt2719_reconfig_SOURCES = wt2719_reconfig/main.c
noinst_PROGRAMS += test_wt2719_reconfig

test_wt2834_join_bloom_fix_SOURCES = wt2834_join_bloom_fix/main.c
noinst_PROGRAMS += test_wt2834_join_bloom_fix

test_wt2853_perf_SOURCES = wt2853_perf/main.c
noinst_PROGRAMS += test_wt2853_perf

test_wt2909_checkpoint_integrity_SOURCES = wt2909_checkpoint_integrity/main.c
noinst_PROGRAMS += test_wt2909_checkpoint_integrity

test_wt2999_join_extractor_SOURCES = wt2999_join_extractor/main.c
noinst_PROGRAMS += test_wt2999_join_extractor

test_wt3120_filesys_SOURCES = wt3120_filesys/main.c
noinst_PROGRAMS += test_wt3120_filesys

test_wt3135_search_near_collator_SOURCES = wt3135_search_near_collator/main.c
noinst_PROGRAMS += test_wt3135_search_near_collator

test_wt3184_dup_index_collator_SOURCES = wt3184_dup_index_collator/main.c
noinst_PROGRAMS += test_wt3184_dup_index_collator

# Run this during a "make check" smoke test.
TESTS = $(noinst_PROGRAMS)
LOG_COMPILER = $(TEST_WRAPPER)

clean-local:
	rm -rf WT_TEST.* *.core
