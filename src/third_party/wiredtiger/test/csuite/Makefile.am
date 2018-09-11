AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir)/src/include \
    -I$(top_srcdir)/test/utility
LDADD = $(top_builddir)/test/utility/libtest_util.la \
    $(top_builddir)/libwiredtiger.la
AM_LDFLAGS = -static

all_TESTS=
noinst_PROGRAMS=

test_random_abort_SOURCES = random_abort/main.c
noinst_PROGRAMS += test_random_abort
all_TESTS += random_abort/smoke.sh

test_rwlock_SOURCES = rwlock/main.c
noinst_PROGRAMS += test_rwlock
all_TESTS += test_rwlock

test_schema_abort_SOURCES = schema_abort/main.c
noinst_PROGRAMS += test_schema_abort
all_TESTS += schema_abort/smoke.sh

test_scope_SOURCES = scope/main.c
noinst_PROGRAMS += test_scope
all_TESTS += test_scope

test_timestamp_abort_SOURCES = timestamp_abort/main.c
noinst_PROGRAMS += test_timestamp_abort
all_TESTS += timestamp_abort/smoke.sh

test_truncated_log_SOURCES = truncated_log/main.c
noinst_PROGRAMS += test_truncated_log
all_TESTS += test_truncated_log

test_wt1965_col_efficiency_SOURCES = wt1965_col_efficiency/main.c
noinst_PROGRAMS += test_wt1965_col_efficiency
all_TESTS += test_wt1965_col_efficiency

test_wt2403_lsm_workload_SOURCES = wt2403_lsm_workload/main.c
noinst_PROGRAMS += test_wt2403_lsm_workload
all_TESTS += test_wt2403_lsm_workload

test_wt2246_col_append_SOURCES = wt2246_col_append/main.c
noinst_PROGRAMS += test_wt2246_col_append
all_TESTS += test_wt2246_col_append

test_wt2323_join_visibility_SOURCES = wt2323_join_visibility/main.c
noinst_PROGRAMS += test_wt2323_join_visibility
all_TESTS += test_wt2323_join_visibility

test_wt2535_insert_race_SOURCES = wt2535_insert_race/main.c
noinst_PROGRAMS += test_wt2535_insert_race
all_TESTS += test_wt2535_insert_race

test_wt2447_join_main_table_SOURCES = wt2447_join_main_table/main.c
noinst_PROGRAMS += test_wt2447_join_main_table
all_TESTS += test_wt2447_join_main_table

test_wt2695_checksum_SOURCES = wt2695_checksum/main.c
noinst_PROGRAMS += test_wt2695_checksum
all_TESTS += test_wt2695_checksum

test_wt2592_join_schema_SOURCES = wt2592_join_schema/main.c
noinst_PROGRAMS += test_wt2592_join_schema
all_TESTS += test_wt2592_join_schema

test_wt2719_reconfig_SOURCES = wt2719_reconfig/main.c
noinst_PROGRAMS += test_wt2719_reconfig
all_TESTS += test_wt2719_reconfig

test_wt2834_join_bloom_fix_SOURCES = wt2834_join_bloom_fix/main.c
noinst_PROGRAMS += test_wt2834_join_bloom_fix
all_TESTS += test_wt2834_join_bloom_fix

test_wt2853_perf_SOURCES = wt2853_perf/main.c
noinst_PROGRAMS += test_wt2853_perf
all_TESTS += test_wt2853_perf

test_wt2909_checkpoint_integrity_SOURCES = wt2909_checkpoint_integrity/main.c
noinst_PROGRAMS += test_wt2909_checkpoint_integrity
all_TESTS += test_wt2909_checkpoint_integrity

test_wt2999_join_extractor_SOURCES = wt2999_join_extractor/main.c
noinst_PROGRAMS += test_wt2999_join_extractor
all_TESTS += test_wt2999_join_extractor

test_wt3120_filesys_SOURCES = wt3120_filesys/main.c
noinst_PROGRAMS += test_wt3120_filesys
all_TESTS += test_wt3120_filesys

test_wt3135_search_near_collator_SOURCES = wt3135_search_near_collator/main.c
noinst_PROGRAMS += test_wt3135_search_near_collator
all_TESTS += test_wt3135_search_near_collator

test_wt3184_dup_index_collator_SOURCES = wt3184_dup_index_collator/main.c
noinst_PROGRAMS += test_wt3184_dup_index_collator
all_TESTS += test_wt3184_dup_index_collator

test_wt3338_partial_update_SOURCES = wt3338_partial_update/main.c
noinst_PROGRAMS += test_wt3338_partial_update
all_TESTS += test_wt3338_partial_update

test_wt3363_checkpoint_op_races_SOURCES = wt3363_checkpoint_op_races/main.c
noinst_PROGRAMS += test_wt3363_checkpoint_op_races
all_TESTS += test_wt3363_checkpoint_op_races

test_wt3874_pad_byte_collator_SOURCES = wt3874_pad_byte_collator/main.c
noinst_PROGRAMS += test_wt3874_pad_byte_collator
all_TESTS += test_wt3874_pad_byte_collator

test_wt4105_large_doc_small_upd_SOURCES = wt4105_large_doc_small_upd/main.c
noinst_PROGRAMS += test_wt4105_large_doc_small_upd
all_TESTS += test_wt4105_large_doc_small_upd

test_wt4117_checksum_SOURCES = wt4117_checksum/main.c
noinst_PROGRAMS += test_wt4117_checksum
all_TESTS += test_wt4117_checksum

test_wt4156_metadata_salvage_SOURCES = wt4156_metadata_salvage/main.c
noinst_PROGRAMS += test_wt4156_metadata_salvage
all_TESTS += test_wt4156_metadata_salvage

# Run this during a "make check" smoke test.
TESTS = $(all_TESTS)
LOG_COMPILER = $(TEST_WRAPPER)

clean-local:
	rm -rf WT_TEST.* *.core
