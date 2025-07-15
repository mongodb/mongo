#!/bin/bash

# Complain about unused statistics fields.
. `dirname -- ${BASH_SOURCE[0]}`/common_functions.sh
setup_trap
cd_dist
use_pygrep

# List of files to search: skip stat.c, it lists all of the fields by
# definition.
l=$(
    sed \
        -e '/src\/support\/stat.c/d' \
        -e '/^[a-z]/!d' \
        -e 's/[	 ].*$//' \
        -e 's,^,../,' filelist
    ls -1 ../src/*/*_inline.h ../src/include/os.h
)

[[ -z "$( { echo ../src/include/stat.h; echo "$l"; } | filter_if_fast ../)" ]] && exit 0

{
# Get the list of statistics fields.
search=`sed -n '/^struct/,/^}/p' ../src/include/stat.h | sed \
    -e 's/^    int64_t \([a-z_]*\);$/\1/p' \
    -e d |
    sort`

# There are some fields that are used, but we can't detect it.
cat << UNUSED_STAT_FIELDS
btree_clean_checkpoint_timer
checkpoints_total_failed
checkpoints_total_succeed
compress_read_ratio_hist_max
compress_write_ratio_hist_max
live_restore_hist_source_read_latency_total_msecs
lock_btree_page_count
lock_btree_page_wait_application
lock_btree_page_wait_internal
lock_checkpoint_count
lock_checkpoint_wait_application
lock_checkpoint_wait_internal
lock_dhandle_read_count
lock_dhandle_wait
lock_dhandle_wait_application
lock_dhandle_wait_internal
lock_dhandle_write_count
lock_metadata_count
lock_metadata_wait_application
lock_metadata_wait_internal
lock_schema_count
lock_schema_wait
lock_schema_wait_application
lock_schema_wait_internal
lock_table_read_count
lock_table_wait_application
lock_table_wait_internal
lock_table_write_count
lock_txn_global_read_count
lock_txn_global_wait_application
lock_txn_global_wait_internal
lock_txn_global_write_count
perf_hist_bmread_latency_total_msecs
perf_hist_bmwrite_latency_total_msecs
perf_hist_disaggbmread_latency_total_usecs
perf_hist_disaggbmwrite_latency_total_usecs
perf_hist_internal_reconstruct_latency_total_usecs
perf_hist_fsread_latency_total_msecs
perf_hist_fswrite_latency_total_msecs
perf_hist_leaf_reconstruct_latency_total_usecs
perf_hist_opread_latency_total_usecs
perf_hist_opwrite_latency_total_usecs
txn_rts_upd_aborted_dryrun
UNUSED_STAT_FIELDS

echo "$search"
cat $l | $FGREP -wo "$search"
} | sort | uniq -u > $t

test -s $t && {
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo 'unused statistics fields'
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    cat $t
    exit 1
}
exit 0
