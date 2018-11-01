/* DO NOT EDIT: automatically built by dist/api_config.py. */

#include "wt_internal.h"

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_async_new_op[] = {
	{ "append", "boolean", NULL, NULL, NULL, 0 },
	{ "overwrite", "boolean", NULL, NULL, NULL, 0 },
	{ "raw", "boolean", NULL, NULL, NULL, 0 },
	{ "timeout", "int", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_close[] = {
	{ "leak_memory", "boolean", NULL, NULL, NULL, 0 },
	{ "use_timestamp", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_debug_info[] = {
	{ "cache", "boolean", NULL, NULL, NULL, 0 },
	{ "cursors", "boolean", NULL, NULL, NULL, 0 },
	{ "handles", "boolean", NULL, NULL, NULL, 0 },
	{ "log", "boolean", NULL, NULL, NULL, 0 },
	{ "sessions", "boolean", NULL, NULL, NULL, 0 },
	{ "txn", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_load_extension[] = {
	{ "config", "string", NULL, NULL, NULL, 0 },
	{ "early_load", "boolean", NULL, NULL, NULL, 0 },
	{ "entry", "string", NULL, NULL, NULL, 0 },
	{ "terminate", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_open_session[] = {
	{ "cache_cursors", "boolean", NULL, NULL, NULL, 0 },
	{ "ignore_cache_size", "boolean", NULL, NULL, NULL, 0 },
	{ "isolation", "string",
	    NULL, "choices=[\"read-uncommitted\",\"read-committed\","
	    "\"snapshot\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_query_timestamp[] = {
	{ "get", "string",
	    NULL, "choices=[\"all_committed\",\"last_checkpoint\",\"oldest\""
	    ",\"oldest_reader\",\"pinned\",\"recovery\",\"stable\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_wiredtiger_open_async_subconfigs[] = {
	{ "enabled", "boolean", NULL, NULL, NULL, 0 },
	{ "ops_max", "int", NULL, "min=1,max=4096", NULL, 0 },
	{ "threads", "int", NULL, "min=1,max=20", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_wiredtiger_open_checkpoint_subconfigs[] = {
	{ "log_size", "int", NULL, "min=0,max=2GB", NULL, 0 },
	{ "wait", "int", NULL, "min=0,max=100000", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_WT_CONNECTION_reconfigure_compatibility_subconfigs[] = {
	{ "release", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_wiredtiger_open_eviction_subconfigs[] = {
	{ "threads_max", "int", NULL, "min=1,max=20", NULL, 0 },
	{ "threads_min", "int", NULL, "min=1,max=20", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_wiredtiger_open_file_manager_subconfigs[] = {
	{ "close_handle_minimum", "int", NULL, "min=0", NULL, 0 },
	{ "close_idle_time", "int",
	    NULL, "min=0,max=100000",
	    NULL, 0 },
	{ "close_scan_interval", "int",
	    NULL, "min=1,max=100000",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_WT_CONNECTION_reconfigure_log_subconfigs[] = {
	{ "archive", "boolean", NULL, NULL, NULL, 0 },
	{ "prealloc", "boolean", NULL, NULL, NULL, 0 },
	{ "zero_fill", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_wiredtiger_open_lsm_manager_subconfigs[] = {
	{ "merge", "boolean", NULL, NULL, NULL, 0 },
	{ "worker_thread_max", "int", NULL, "min=3,max=20", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_wiredtiger_open_operation_tracking_subconfigs[] = {
	{ "enabled", "boolean", NULL, NULL, NULL, 0 },
	{ "path", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_wiredtiger_open_shared_cache_subconfigs[] = {
	{ "chunk", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "name", "string", NULL, NULL, NULL, 0 },
	{ "quota", "int", NULL, NULL, NULL, 0 },
	{ "reserve", "int", NULL, NULL, NULL, 0 },
	{ "size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_WT_CONNECTION_reconfigure_statistics_log_subconfigs[] = {
	{ "json", "boolean", NULL, NULL, NULL, 0 },
	{ "on_close", "boolean", NULL, NULL, NULL, 0 },
	{ "sources", "list", NULL, NULL, NULL, 0 },
	{ "timestamp", "string", NULL, NULL, NULL, 0 },
	{ "wait", "int", NULL, "min=0,max=100000", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_reconfigure[] = {
	{ "async", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_async_subconfigs, 3 },
	{ "cache_max_wait_ms", "int", NULL, "min=0", NULL, 0 },
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_checkpoint_subconfigs, 2 },
	{ "compatibility", "category",
	    NULL, NULL,
	    confchk_WT_CONNECTION_reconfigure_compatibility_subconfigs, 1 },
	{ "error_prefix", "string", NULL, NULL, NULL, 0 },
	{ "eviction", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_eviction_subconfigs, 2 },
	{ "eviction_checkpoint_target", "int",
	    NULL, "min=0,max=10TB",
	    NULL, 0 },
	{ "eviction_dirty_target", "int",
	    NULL, "min=1,max=10TB",
	    NULL, 0 },
	{ "eviction_dirty_trigger", "int",
	    NULL, "min=1,max=10TB",
	    NULL, 0 },
	{ "eviction_target", "int", NULL, "min=10,max=10TB", NULL, 0 },
	{ "eviction_trigger", "int",
	    NULL, "min=10,max=10TB",
	    NULL, 0 },
	{ "file_manager", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_file_manager_subconfigs, 3 },
	{ "log", "category",
	    NULL, NULL,
	    confchk_WT_CONNECTION_reconfigure_log_subconfigs, 3 },
	{ "lsm_manager", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_lsm_manager_subconfigs, 2 },
	{ "lsm_merge", "boolean", NULL, NULL, NULL, 0 },
	{ "operation_tracking", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_operation_tracking_subconfigs, 2 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_shared_cache_subconfigs, 5 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"cache_walk\",\"fast\",\"none\","
	    "\"clear\",\"tree_walk\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_WT_CONNECTION_reconfigure_statistics_log_subconfigs, 5 },
	{ "timing_stress_for_test", "list",
	    NULL, "choices=[\"checkpoint_slow\",\"lookaside_sweep_race\","
	    "\"split_1\",\"split_2\",\"split_3\",\"split_4\",\"split_5\","
	    "\"split_6\",\"split_7\",\"split_8\"]",
	    NULL, 0 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\","
	    "\"checkpoint_progress\",\"compact\",\"error_returns\",\"evict\","
	    "\"evict_stuck\",\"evictserver\",\"fileops\",\"handleops\","
	    "\"log\",\"lookaside\",\"lookaside_activity\",\"lsm\","
	    "\"lsm_manager\",\"metadata\",\"mutex\",\"overflow\",\"read\","
	    "\"rebalance\",\"reconcile\",\"recovery\",\"recovery_progress\","
	    "\"salvage\",\"shared_cache\",\"split\",\"temporary\","
	    "\"thread_group\",\"timestamp\",\"transaction\",\"verify\","
	    "\"version\",\"write\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_set_timestamp[] = {
	{ "commit_timestamp", "string", NULL, NULL, NULL, 0 },
	{ "force", "boolean", NULL, NULL, NULL, 0 },
	{ "oldest_timestamp", "string", NULL, NULL, NULL, 0 },
	{ "stable_timestamp", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_CURSOR_reconfigure[] = {
	{ "append", "boolean", NULL, NULL, NULL, 0 },
	{ "overwrite", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_assert_subconfigs[] = {
	{ "commit_timestamp", "string",
	    NULL, "choices=[\"always\",\"key_consistent\",\"never\","
	    "\"none\"]",
	    NULL, 0 },
	{ "read_timestamp", "string",
	    NULL, "choices=[\"always\",\"never\",\"none\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_WT_SESSION_create_log_subconfigs[] = {
	{ "enabled", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_alter[] = {
	{ "access_pattern_hint", "string",
	    NULL, "choices=[\"none\",\"random\",\"sequential\"]",
	    NULL, 0 },
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
	{ "assert", "category",
	    NULL, NULL,
	    confchk_assert_subconfigs, 2 },
	{ "cache_resident", "boolean", NULL, NULL, NULL, 0 },
	{ "exclusive_refreshed", "boolean", NULL, NULL, NULL, 0 },
	{ "log", "category",
	    NULL, NULL,
	    confchk_WT_SESSION_create_log_subconfigs, 1 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_begin_transaction[] = {
	{ "ignore_prepare", "boolean", NULL, NULL, NULL, 0 },
	{ "isolation", "string",
	    NULL, "choices=[\"read-uncommitted\",\"read-committed\","
	    "\"snapshot\"]",
	    NULL, 0 },
	{ "name", "string", NULL, NULL, NULL, 0 },
	{ "priority", "int", NULL, "min=-100,max=100", NULL, 0 },
	{ "read_timestamp", "string", NULL, NULL, NULL, 0 },
	{ "round_to_oldest", "boolean", NULL, NULL, NULL, 0 },
	{ "snapshot", "string", NULL, NULL, NULL, 0 },
	{ "sync", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_checkpoint[] = {
	{ "drop", "list", NULL, NULL, NULL, 0 },
	{ "force", "boolean", NULL, NULL, NULL, 0 },
	{ "name", "string", NULL, NULL, NULL, 0 },
	{ "target", "list", NULL, NULL, NULL, 0 },
	{ "use_timestamp", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_commit_transaction[] = {
	{ "commit_timestamp", "string", NULL, NULL, NULL, 0 },
	{ "sync", "string",
	    NULL, "choices=[\"background\",\"off\",\"on\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_compact[] = {
	{ "timeout", "int", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_WT_SESSION_create_encryption_subconfigs[] = {
	{ "keyid", "string", NULL, NULL, NULL, 0 },
	{ "name", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_WT_SESSION_create_merge_custom_subconfigs[] = {
	{ "prefix", "string", NULL, NULL, NULL, 0 },
	{ "start_generation", "int", NULL, "min=0,max=10", NULL, 0 },
	{ "suffix", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_WT_SESSION_create_lsm_subconfigs[] = {
	{ "auto_throttle", "boolean", NULL, NULL, NULL, 0 },
	{ "bloom", "boolean", NULL, NULL, NULL, 0 },
	{ "bloom_bit_count", "int", NULL, "min=2,max=1000", NULL, 0 },
	{ "bloom_config", "string", NULL, NULL, NULL, 0 },
	{ "bloom_hash_count", "int", NULL, "min=2,max=100", NULL, 0 },
	{ "bloom_oldest", "boolean", NULL, NULL, NULL, 0 },
	{ "chunk_count_limit", "int", NULL, NULL, NULL, 0 },
	{ "chunk_max", "int", NULL, "min=100MB,max=10TB", NULL, 0 },
	{ "chunk_size", "int", NULL, "min=512K,max=500MB", NULL, 0 },
	{ "merge_custom", "category",
	    NULL, NULL,
	    confchk_WT_SESSION_create_merge_custom_subconfigs, 3 },
	{ "merge_max", "int", NULL, "min=2,max=100", NULL, 0 },
	{ "merge_min", "int", NULL, "max=100", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_create[] = {
	{ "access_pattern_hint", "string",
	    NULL, "choices=[\"none\",\"random\",\"sequential\"]",
	    NULL, 0 },
	{ "allocation_size", "int",
	    NULL, "min=512B,max=128MB",
	    NULL, 0 },
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
	{ "assert", "category",
	    NULL, NULL,
	    confchk_assert_subconfigs, 2 },
	{ "block_allocation", "string",
	    NULL, "choices=[\"first\",\"best\"]",
	    NULL, 0 },
	{ "block_compressor", "string", NULL, NULL, NULL, 0 },
	{ "cache_resident", "boolean", NULL, NULL, NULL, 0 },
	{ "checksum", "string",
	    NULL, "choices=[\"on\",\"off\",\"uncompressed\"]",
	    NULL, 0 },
	{ "colgroups", "list", NULL, NULL, NULL, 0 },
	{ "collator", "string", NULL, NULL, NULL, 0 },
	{ "columns", "list", NULL, NULL, NULL, 0 },
	{ "dictionary", "int", NULL, "min=0", NULL, 0 },
	{ "encryption", "category",
	    NULL, NULL,
	    confchk_WT_SESSION_create_encryption_subconfigs, 2 },
	{ "exclusive", "boolean", NULL, NULL, NULL, 0 },
	{ "extractor", "string", NULL, NULL, NULL, 0 },
	{ "format", "string", NULL, "choices=[\"btree\"]", NULL, 0 },
	{ "huffman_key", "string", NULL, NULL, NULL, 0 },
	{ "huffman_value", "string", NULL, NULL, NULL, 0 },
	{ "ignore_in_memory_cache_size", "boolean",
	    NULL, NULL,
	    NULL, 0 },
	{ "immutable", "boolean", NULL, NULL, NULL, 0 },
	{ "internal_item_max", "int", NULL, "min=0", NULL, 0 },
	{ "internal_key_max", "int", NULL, "min=0", NULL, 0 },
	{ "internal_key_truncate", "boolean", NULL, NULL, NULL, 0 },
	{ "internal_page_max", "int",
	    NULL, "min=512B,max=512MB",
	    NULL, 0 },
	{ "key_format", "format", __wt_struct_confchk, NULL, NULL, 0 },
	{ "key_gap", "int", NULL, "min=0", NULL, 0 },
	{ "leaf_item_max", "int", NULL, "min=0", NULL, 0 },
	{ "leaf_key_max", "int", NULL, "min=0", NULL, 0 },
	{ "leaf_page_max", "int",
	    NULL, "min=512B,max=512MB",
	    NULL, 0 },
	{ "leaf_value_max", "int", NULL, "min=0", NULL, 0 },
	{ "log", "category",
	    NULL, NULL,
	    confchk_WT_SESSION_create_log_subconfigs, 1 },
	{ "lsm", "category",
	    NULL, NULL,
	    confchk_WT_SESSION_create_lsm_subconfigs, 12 },
	{ "memory_page_image_max", "int", NULL, "min=0", NULL, 0 },
	{ "memory_page_max", "int",
	    NULL, "min=512B,max=10TB",
	    NULL, 0 },
	{ "os_cache_dirty_max", "int", NULL, "min=0", NULL, 0 },
	{ "os_cache_max", "int", NULL, "min=0", NULL, 0 },
	{ "prefix_compression", "boolean", NULL, NULL, NULL, 0 },
	{ "prefix_compression_min", "int", NULL, "min=0", NULL, 0 },
	{ "source", "string", NULL, NULL, NULL, 0 },
	{ "split_deepen_min_child", "int", NULL, NULL, NULL, 0 },
	{ "split_deepen_per_child", "int", NULL, NULL, NULL, 0 },
	{ "split_pct", "int", NULL, "min=50,max=100", NULL, 0 },
	{ "type", "string", NULL, NULL, NULL, 0 },
	{ "value_format", "format",
	    __wt_struct_confchk, NULL,
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_drop[] = {
	{ "checkpoint_wait", "boolean", NULL, NULL, NULL, 0 },
	{ "force", "boolean", NULL, NULL, NULL, 0 },
	{ "lock_wait", "boolean", NULL, NULL, NULL, 0 },
	{ "remove_files", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_join[] = {
	{ "bloom_bit_count", "int", NULL, "min=2,max=1000", NULL, 0 },
	{ "bloom_false_positives", "boolean", NULL, NULL, NULL, 0 },
	{ "bloom_hash_count", "int", NULL, "min=2,max=100", NULL, 0 },
	{ "compare", "string",
	    NULL, "choices=[\"eq\",\"ge\",\"gt\",\"le\",\"lt\"]",
	    NULL, 0 },
	{ "count", "int", NULL, NULL, NULL, 0 },
	{ "operation", "string",
	    NULL, "choices=[\"and\",\"or\"]",
	    NULL, 0 },
	{ "strategy", "string",
	    NULL, "choices=[\"bloom\",\"default\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_log_flush[] = {
	{ "sync", "string",
	    NULL, "choices=[\"background\",\"off\",\"on\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_open_cursor[] = {
	{ "append", "boolean", NULL, NULL, NULL, 0 },
	{ "bulk", "string", NULL, NULL, NULL, 0 },
	{ "checkpoint", "string", NULL, NULL, NULL, 0 },
	{ "checkpoint_wait", "boolean", NULL, NULL, NULL, 0 },
	{ "dump", "string",
	    NULL, "choices=[\"hex\",\"json\",\"print\"]",
	    NULL, 0 },
	{ "next_random", "boolean", NULL, NULL, NULL, 0 },
	{ "next_random_sample_size", "string", NULL, NULL, NULL, 0 },
	{ "overwrite", "boolean", NULL, NULL, NULL, 0 },
	{ "raw", "boolean", NULL, NULL, NULL, 0 },
	{ "read_once", "boolean", NULL, NULL, NULL, 0 },
	{ "readonly", "boolean", NULL, NULL, NULL, 0 },
	{ "skip_sort_check", "boolean", NULL, NULL, NULL, 0 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"cache_walk\",\"fast\",\"clear\","
	    "\"size\",\"tree_walk\"]",
	    NULL, 0 },
	{ "target", "list", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_prepare_transaction[] = {
	{ "prepare_timestamp", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_query_timestamp[] = {
	{ "get", "string",
	    NULL, "choices=[\"commit\",\"first_commit\",\"prepare\","
	    "\"read\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_reconfigure[] = {
	{ "cache_cursors", "boolean", NULL, NULL, NULL, 0 },
	{ "ignore_cache_size", "boolean", NULL, NULL, NULL, 0 },
	{ "isolation", "string",
	    NULL, "choices=[\"read-uncommitted\",\"read-committed\","
	    "\"snapshot\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_salvage[] = {
	{ "force", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_WT_SESSION_snapshot_drop_subconfigs[] = {
	{ "all", "boolean", NULL, NULL, NULL, 0 },
	{ "before", "string", NULL, NULL, NULL, 0 },
	{ "names", "list", NULL, NULL, NULL, 0 },
	{ "to", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_snapshot[] = {
	{ "drop", "category",
	    NULL, NULL,
	    confchk_WT_SESSION_snapshot_drop_subconfigs, 4 },
	{ "include_updates", "boolean", NULL, NULL, NULL, 0 },
	{ "name", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_timestamp_transaction[] = {
	{ "commit_timestamp", "string", NULL, NULL, NULL, 0 },
	{ "read_timestamp", "string", NULL, NULL, NULL, 0 },
	{ "round_to_oldest", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_transaction_sync[] = {
	{ "timeout_ms", "int", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_verify[] = {
	{ "dump_address", "boolean", NULL, NULL, NULL, 0 },
	{ "dump_blocks", "boolean", NULL, NULL, NULL, 0 },
	{ "dump_layout", "boolean", NULL, NULL, NULL, 0 },
	{ "dump_offsets", "list", NULL, NULL, NULL, 0 },
	{ "dump_pages", "boolean", NULL, NULL, NULL, 0 },
	{ "strict", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_colgroup_meta[] = {
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
	{ "collator", "string", NULL, NULL, NULL, 0 },
	{ "columns", "list", NULL, NULL, NULL, 0 },
	{ "source", "string", NULL, NULL, NULL, 0 },
	{ "type", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_file_config[] = {
	{ "access_pattern_hint", "string",
	    NULL, "choices=[\"none\",\"random\",\"sequential\"]",
	    NULL, 0 },
	{ "allocation_size", "int",
	    NULL, "min=512B,max=128MB",
	    NULL, 0 },
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
	{ "assert", "category",
	    NULL, NULL,
	    confchk_assert_subconfigs, 2 },
	{ "block_allocation", "string",
	    NULL, "choices=[\"first\",\"best\"]",
	    NULL, 0 },
	{ "block_compressor", "string", NULL, NULL, NULL, 0 },
	{ "cache_resident", "boolean", NULL, NULL, NULL, 0 },
	{ "checksum", "string",
	    NULL, "choices=[\"on\",\"off\",\"uncompressed\"]",
	    NULL, 0 },
	{ "collator", "string", NULL, NULL, NULL, 0 },
	{ "columns", "list", NULL, NULL, NULL, 0 },
	{ "dictionary", "int", NULL, "min=0", NULL, 0 },
	{ "encryption", "category",
	    NULL, NULL,
	    confchk_WT_SESSION_create_encryption_subconfigs, 2 },
	{ "format", "string", NULL, "choices=[\"btree\"]", NULL, 0 },
	{ "huffman_key", "string", NULL, NULL, NULL, 0 },
	{ "huffman_value", "string", NULL, NULL, NULL, 0 },
	{ "ignore_in_memory_cache_size", "boolean",
	    NULL, NULL,
	    NULL, 0 },
	{ "internal_item_max", "int", NULL, "min=0", NULL, 0 },
	{ "internal_key_max", "int", NULL, "min=0", NULL, 0 },
	{ "internal_key_truncate", "boolean", NULL, NULL, NULL, 0 },
	{ "internal_page_max", "int",
	    NULL, "min=512B,max=512MB",
	    NULL, 0 },
	{ "key_format", "format", __wt_struct_confchk, NULL, NULL, 0 },
	{ "key_gap", "int", NULL, "min=0", NULL, 0 },
	{ "leaf_item_max", "int", NULL, "min=0", NULL, 0 },
	{ "leaf_key_max", "int", NULL, "min=0", NULL, 0 },
	{ "leaf_page_max", "int",
	    NULL, "min=512B,max=512MB",
	    NULL, 0 },
	{ "leaf_value_max", "int", NULL, "min=0", NULL, 0 },
	{ "log", "category",
	    NULL, NULL,
	    confchk_WT_SESSION_create_log_subconfigs, 1 },
	{ "memory_page_image_max", "int", NULL, "min=0", NULL, 0 },
	{ "memory_page_max", "int",
	    NULL, "min=512B,max=10TB",
	    NULL, 0 },
	{ "os_cache_dirty_max", "int", NULL, "min=0", NULL, 0 },
	{ "os_cache_max", "int", NULL, "min=0", NULL, 0 },
	{ "prefix_compression", "boolean", NULL, NULL, NULL, 0 },
	{ "prefix_compression_min", "int", NULL, "min=0", NULL, 0 },
	{ "split_deepen_min_child", "int", NULL, NULL, NULL, 0 },
	{ "split_deepen_per_child", "int", NULL, NULL, NULL, 0 },
	{ "split_pct", "int", NULL, "min=50,max=100", NULL, 0 },
	{ "value_format", "format",
	    __wt_struct_confchk, NULL,
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_file_meta[] = {
	{ "access_pattern_hint", "string",
	    NULL, "choices=[\"none\",\"random\",\"sequential\"]",
	    NULL, 0 },
	{ "allocation_size", "int",
	    NULL, "min=512B,max=128MB",
	    NULL, 0 },
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
	{ "assert", "category",
	    NULL, NULL,
	    confchk_assert_subconfigs, 2 },
	{ "block_allocation", "string",
	    NULL, "choices=[\"first\",\"best\"]",
	    NULL, 0 },
	{ "block_compressor", "string", NULL, NULL, NULL, 0 },
	{ "cache_resident", "boolean", NULL, NULL, NULL, 0 },
	{ "checkpoint", "string", NULL, NULL, NULL, 0 },
	{ "checkpoint_lsn", "string", NULL, NULL, NULL, 0 },
	{ "checksum", "string",
	    NULL, "choices=[\"on\",\"off\",\"uncompressed\"]",
	    NULL, 0 },
	{ "collator", "string", NULL, NULL, NULL, 0 },
	{ "columns", "list", NULL, NULL, NULL, 0 },
	{ "dictionary", "int", NULL, "min=0", NULL, 0 },
	{ "encryption", "category",
	    NULL, NULL,
	    confchk_WT_SESSION_create_encryption_subconfigs, 2 },
	{ "format", "string", NULL, "choices=[\"btree\"]", NULL, 0 },
	{ "huffman_key", "string", NULL, NULL, NULL, 0 },
	{ "huffman_value", "string", NULL, NULL, NULL, 0 },
	{ "id", "string", NULL, NULL, NULL, 0 },
	{ "ignore_in_memory_cache_size", "boolean",
	    NULL, NULL,
	    NULL, 0 },
	{ "internal_item_max", "int", NULL, "min=0", NULL, 0 },
	{ "internal_key_max", "int", NULL, "min=0", NULL, 0 },
	{ "internal_key_truncate", "boolean", NULL, NULL, NULL, 0 },
	{ "internal_page_max", "int",
	    NULL, "min=512B,max=512MB",
	    NULL, 0 },
	{ "key_format", "format", __wt_struct_confchk, NULL, NULL, 0 },
	{ "key_gap", "int", NULL, "min=0", NULL, 0 },
	{ "leaf_item_max", "int", NULL, "min=0", NULL, 0 },
	{ "leaf_key_max", "int", NULL, "min=0", NULL, 0 },
	{ "leaf_page_max", "int",
	    NULL, "min=512B,max=512MB",
	    NULL, 0 },
	{ "leaf_value_max", "int", NULL, "min=0", NULL, 0 },
	{ "log", "category",
	    NULL, NULL,
	    confchk_WT_SESSION_create_log_subconfigs, 1 },
	{ "memory_page_image_max", "int", NULL, "min=0", NULL, 0 },
	{ "memory_page_max", "int",
	    NULL, "min=512B,max=10TB",
	    NULL, 0 },
	{ "os_cache_dirty_max", "int", NULL, "min=0", NULL, 0 },
	{ "os_cache_max", "int", NULL, "min=0", NULL, 0 },
	{ "prefix_compression", "boolean", NULL, NULL, NULL, 0 },
	{ "prefix_compression_min", "int", NULL, "min=0", NULL, 0 },
	{ "split_deepen_min_child", "int", NULL, NULL, NULL, 0 },
	{ "split_deepen_per_child", "int", NULL, NULL, NULL, 0 },
	{ "split_pct", "int", NULL, "min=50,max=100", NULL, 0 },
	{ "value_format", "format",
	    __wt_struct_confchk, NULL,
	    NULL, 0 },
	{ "version", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_index_meta[] = {
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
	{ "collator", "string", NULL, NULL, NULL, 0 },
	{ "columns", "list", NULL, NULL, NULL, 0 },
	{ "extractor", "string", NULL, NULL, NULL, 0 },
	{ "immutable", "boolean", NULL, NULL, NULL, 0 },
	{ "index_key_columns", "int", NULL, NULL, NULL, 0 },
	{ "key_format", "format", __wt_struct_confchk, NULL, NULL, 0 },
	{ "source", "string", NULL, NULL, NULL, 0 },
	{ "type", "string", NULL, NULL, NULL, 0 },
	{ "value_format", "format",
	    __wt_struct_confchk, NULL,
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_lsm_meta[] = {
	{ "access_pattern_hint", "string",
	    NULL, "choices=[\"none\",\"random\",\"sequential\"]",
	    NULL, 0 },
	{ "allocation_size", "int",
	    NULL, "min=512B,max=128MB",
	    NULL, 0 },
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
	{ "assert", "category",
	    NULL, NULL,
	    confchk_assert_subconfigs, 2 },
	{ "block_allocation", "string",
	    NULL, "choices=[\"first\",\"best\"]",
	    NULL, 0 },
	{ "block_compressor", "string", NULL, NULL, NULL, 0 },
	{ "cache_resident", "boolean", NULL, NULL, NULL, 0 },
	{ "checksum", "string",
	    NULL, "choices=[\"on\",\"off\",\"uncompressed\"]",
	    NULL, 0 },
	{ "chunks", "string", NULL, NULL, NULL, 0 },
	{ "collator", "string", NULL, NULL, NULL, 0 },
	{ "columns", "list", NULL, NULL, NULL, 0 },
	{ "dictionary", "int", NULL, "min=0", NULL, 0 },
	{ "encryption", "category",
	    NULL, NULL,
	    confchk_WT_SESSION_create_encryption_subconfigs, 2 },
	{ "format", "string", NULL, "choices=[\"btree\"]", NULL, 0 },
	{ "huffman_key", "string", NULL, NULL, NULL, 0 },
	{ "huffman_value", "string", NULL, NULL, NULL, 0 },
	{ "ignore_in_memory_cache_size", "boolean",
	    NULL, NULL,
	    NULL, 0 },
	{ "internal_item_max", "int", NULL, "min=0", NULL, 0 },
	{ "internal_key_max", "int", NULL, "min=0", NULL, 0 },
	{ "internal_key_truncate", "boolean", NULL, NULL, NULL, 0 },
	{ "internal_page_max", "int",
	    NULL, "min=512B,max=512MB",
	    NULL, 0 },
	{ "key_format", "format", __wt_struct_confchk, NULL, NULL, 0 },
	{ "key_gap", "int", NULL, "min=0", NULL, 0 },
	{ "last", "string", NULL, NULL, NULL, 0 },
	{ "leaf_item_max", "int", NULL, "min=0", NULL, 0 },
	{ "leaf_key_max", "int", NULL, "min=0", NULL, 0 },
	{ "leaf_page_max", "int",
	    NULL, "min=512B,max=512MB",
	    NULL, 0 },
	{ "leaf_value_max", "int", NULL, "min=0", NULL, 0 },
	{ "log", "category",
	    NULL, NULL,
	    confchk_WT_SESSION_create_log_subconfigs, 1 },
	{ "lsm", "category",
	    NULL, NULL,
	    confchk_WT_SESSION_create_lsm_subconfigs, 12 },
	{ "memory_page_image_max", "int", NULL, "min=0", NULL, 0 },
	{ "memory_page_max", "int",
	    NULL, "min=512B,max=10TB",
	    NULL, 0 },
	{ "old_chunks", "string", NULL, NULL, NULL, 0 },
	{ "os_cache_dirty_max", "int", NULL, "min=0", NULL, 0 },
	{ "os_cache_max", "int", NULL, "min=0", NULL, 0 },
	{ "prefix_compression", "boolean", NULL, NULL, NULL, 0 },
	{ "prefix_compression_min", "int", NULL, "min=0", NULL, 0 },
	{ "split_deepen_min_child", "int", NULL, NULL, NULL, 0 },
	{ "split_deepen_per_child", "int", NULL, NULL, NULL, 0 },
	{ "split_pct", "int", NULL, "min=50,max=100", NULL, 0 },
	{ "value_format", "format",
	    __wt_struct_confchk, NULL,
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_table_meta[] = {
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
	{ "colgroups", "list", NULL, NULL, NULL, 0 },
	{ "collator", "string", NULL, NULL, NULL, 0 },
	{ "columns", "list", NULL, NULL, NULL, 0 },
	{ "key_format", "format", __wt_struct_confchk, NULL, NULL, 0 },
	{ "value_format", "format",
	    __wt_struct_confchk, NULL,
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_wiredtiger_open_compatibility_subconfigs[] = {
	{ "release", "string", NULL, NULL, NULL, 0 },
	{ "require_max", "string", NULL, NULL, NULL, 0 },
	{ "require_min", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_wiredtiger_open_encryption_subconfigs[] = {
	{ "keyid", "string", NULL, NULL, NULL, 0 },
	{ "name", "string", NULL, NULL, NULL, 0 },
	{ "secretkey", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_wiredtiger_open_log_subconfigs[] = {
	{ "archive", "boolean", NULL, NULL, NULL, 0 },
	{ "compressor", "string", NULL, NULL, NULL, 0 },
	{ "enabled", "boolean", NULL, NULL, NULL, 0 },
	{ "file_max", "int", NULL, "min=100KB,max=2GB", NULL, 0 },
	{ "path", "string", NULL, NULL, NULL, 0 },
	{ "prealloc", "boolean", NULL, NULL, NULL, 0 },
	{ "recover", "string",
	    NULL, "choices=[\"error\",\"on\"]",
	    NULL, 0 },
	{ "zero_fill", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_wiredtiger_open_statistics_log_subconfigs[] = {
	{ "json", "boolean", NULL, NULL, NULL, 0 },
	{ "on_close", "boolean", NULL, NULL, NULL, 0 },
	{ "path", "string", NULL, NULL, NULL, 0 },
	{ "sources", "list", NULL, NULL, NULL, 0 },
	{ "timestamp", "string", NULL, NULL, NULL, 0 },
	{ "wait", "int", NULL, "min=0,max=100000", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK
    confchk_wiredtiger_open_transaction_sync_subconfigs[] = {
	{ "enabled", "boolean", NULL, NULL, NULL, 0 },
	{ "method", "string",
	    NULL, "choices=[\"dsync\",\"fsync\",\"none\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_wiredtiger_open[] = {
	{ "async", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_async_subconfigs, 3 },
	{ "buffer_alignment", "int", NULL, "min=-1,max=1MB", NULL, 0 },
	{ "builtin_extension_config", "string", NULL, NULL, NULL, 0 },
	{ "cache_cursors", "boolean", NULL, NULL, NULL, 0 },
	{ "cache_max_wait_ms", "int", NULL, "min=0", NULL, 0 },
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_checkpoint_subconfigs, 2 },
	{ "checkpoint_sync", "boolean", NULL, NULL, NULL, 0 },
	{ "compatibility", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_compatibility_subconfigs, 3 },
	{ "config_base", "boolean", NULL, NULL, NULL, 0 },
	{ "create", "boolean", NULL, NULL, NULL, 0 },
	{ "direct_io", "list",
	    NULL, "choices=[\"checkpoint\",\"data\",\"log\"]",
	    NULL, 0 },
	{ "encryption", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_encryption_subconfigs, 3 },
	{ "error_prefix", "string", NULL, NULL, NULL, 0 },
	{ "eviction", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_eviction_subconfigs, 2 },
	{ "eviction_checkpoint_target", "int",
	    NULL, "min=0,max=10TB",
	    NULL, 0 },
	{ "eviction_dirty_target", "int",
	    NULL, "min=1,max=10TB",
	    NULL, 0 },
	{ "eviction_dirty_trigger", "int",
	    NULL, "min=1,max=10TB",
	    NULL, 0 },
	{ "eviction_target", "int", NULL, "min=10,max=10TB", NULL, 0 },
	{ "eviction_trigger", "int",
	    NULL, "min=10,max=10TB",
	    NULL, 0 },
	{ "exclusive", "boolean", NULL, NULL, NULL, 0 },
	{ "extensions", "list", NULL, NULL, NULL, 0 },
	{ "file_extend", "list",
	    NULL, "choices=[\"data\",\"log\"]",
	    NULL, 0 },
	{ "file_manager", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_file_manager_subconfigs, 3 },
	{ "hazard_max", "int", NULL, "min=15", NULL, 0 },
	{ "in_memory", "boolean", NULL, NULL, NULL, 0 },
	{ "log", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_log_subconfigs, 8 },
	{ "lsm_manager", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_lsm_manager_subconfigs, 2 },
	{ "lsm_merge", "boolean", NULL, NULL, NULL, 0 },
	{ "mmap", "boolean", NULL, NULL, NULL, 0 },
	{ "multiprocess", "boolean", NULL, NULL, NULL, 0 },
	{ "operation_tracking", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_operation_tracking_subconfigs, 2 },
	{ "readonly", "boolean", NULL, NULL, NULL, 0 },
	{ "salvage", "boolean", NULL, NULL, NULL, 0 },
	{ "session_max", "int", NULL, "min=1", NULL, 0 },
	{ "session_scratch_max", "int", NULL, NULL, NULL, 0 },
	{ "session_table_cache", "boolean", NULL, NULL, NULL, 0 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_shared_cache_subconfigs, 5 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"cache_walk\",\"fast\",\"none\","
	    "\"clear\",\"tree_walk\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_statistics_log_subconfigs, 6 },
	{ "timing_stress_for_test", "list",
	    NULL, "choices=[\"checkpoint_slow\",\"lookaside_sweep_race\","
	    "\"split_1\",\"split_2\",\"split_3\",\"split_4\",\"split_5\","
	    "\"split_6\",\"split_7\",\"split_8\"]",
	    NULL, 0 },
	{ "transaction_sync", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_transaction_sync_subconfigs, 2 },
	{ "use_environment", "boolean", NULL, NULL, NULL, 0 },
	{ "use_environment_priv", "boolean", NULL, NULL, NULL, 0 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\","
	    "\"checkpoint_progress\",\"compact\",\"error_returns\",\"evict\","
	    "\"evict_stuck\",\"evictserver\",\"fileops\",\"handleops\","
	    "\"log\",\"lookaside\",\"lookaside_activity\",\"lsm\","
	    "\"lsm_manager\",\"metadata\",\"mutex\",\"overflow\",\"read\","
	    "\"rebalance\",\"reconcile\",\"recovery\",\"recovery_progress\","
	    "\"salvage\",\"shared_cache\",\"split\",\"temporary\","
	    "\"thread_group\",\"timestamp\",\"transaction\",\"verify\","
	    "\"version\",\"write\"]",
	    NULL, 0 },
	{ "write_through", "list",
	    NULL, "choices=[\"data\",\"log\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_all[] = {
	{ "async", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_async_subconfigs, 3 },
	{ "buffer_alignment", "int", NULL, "min=-1,max=1MB", NULL, 0 },
	{ "builtin_extension_config", "string", NULL, NULL, NULL, 0 },
	{ "cache_cursors", "boolean", NULL, NULL, NULL, 0 },
	{ "cache_max_wait_ms", "int", NULL, "min=0", NULL, 0 },
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_checkpoint_subconfigs, 2 },
	{ "checkpoint_sync", "boolean", NULL, NULL, NULL, 0 },
	{ "compatibility", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_compatibility_subconfigs, 3 },
	{ "config_base", "boolean", NULL, NULL, NULL, 0 },
	{ "create", "boolean", NULL, NULL, NULL, 0 },
	{ "direct_io", "list",
	    NULL, "choices=[\"checkpoint\",\"data\",\"log\"]",
	    NULL, 0 },
	{ "encryption", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_encryption_subconfigs, 3 },
	{ "error_prefix", "string", NULL, NULL, NULL, 0 },
	{ "eviction", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_eviction_subconfigs, 2 },
	{ "eviction_checkpoint_target", "int",
	    NULL, "min=0,max=10TB",
	    NULL, 0 },
	{ "eviction_dirty_target", "int",
	    NULL, "min=1,max=10TB",
	    NULL, 0 },
	{ "eviction_dirty_trigger", "int",
	    NULL, "min=1,max=10TB",
	    NULL, 0 },
	{ "eviction_target", "int", NULL, "min=10,max=10TB", NULL, 0 },
	{ "eviction_trigger", "int",
	    NULL, "min=10,max=10TB",
	    NULL, 0 },
	{ "exclusive", "boolean", NULL, NULL, NULL, 0 },
	{ "extensions", "list", NULL, NULL, NULL, 0 },
	{ "file_extend", "list",
	    NULL, "choices=[\"data\",\"log\"]",
	    NULL, 0 },
	{ "file_manager", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_file_manager_subconfigs, 3 },
	{ "hazard_max", "int", NULL, "min=15", NULL, 0 },
	{ "in_memory", "boolean", NULL, NULL, NULL, 0 },
	{ "log", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_log_subconfigs, 8 },
	{ "lsm_manager", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_lsm_manager_subconfigs, 2 },
	{ "lsm_merge", "boolean", NULL, NULL, NULL, 0 },
	{ "mmap", "boolean", NULL, NULL, NULL, 0 },
	{ "multiprocess", "boolean", NULL, NULL, NULL, 0 },
	{ "operation_tracking", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_operation_tracking_subconfigs, 2 },
	{ "readonly", "boolean", NULL, NULL, NULL, 0 },
	{ "salvage", "boolean", NULL, NULL, NULL, 0 },
	{ "session_max", "int", NULL, "min=1", NULL, 0 },
	{ "session_scratch_max", "int", NULL, NULL, NULL, 0 },
	{ "session_table_cache", "boolean", NULL, NULL, NULL, 0 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_shared_cache_subconfigs, 5 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"cache_walk\",\"fast\",\"none\","
	    "\"clear\",\"tree_walk\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_statistics_log_subconfigs, 6 },
	{ "timing_stress_for_test", "list",
	    NULL, "choices=[\"checkpoint_slow\",\"lookaside_sweep_race\","
	    "\"split_1\",\"split_2\",\"split_3\",\"split_4\",\"split_5\","
	    "\"split_6\",\"split_7\",\"split_8\"]",
	    NULL, 0 },
	{ "transaction_sync", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_transaction_sync_subconfigs, 2 },
	{ "use_environment", "boolean", NULL, NULL, NULL, 0 },
	{ "use_environment_priv", "boolean", NULL, NULL, NULL, 0 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\","
	    "\"checkpoint_progress\",\"compact\",\"error_returns\",\"evict\","
	    "\"evict_stuck\",\"evictserver\",\"fileops\",\"handleops\","
	    "\"log\",\"lookaside\",\"lookaside_activity\",\"lsm\","
	    "\"lsm_manager\",\"metadata\",\"mutex\",\"overflow\",\"read\","
	    "\"rebalance\",\"reconcile\",\"recovery\",\"recovery_progress\","
	    "\"salvage\",\"shared_cache\",\"split\",\"temporary\","
	    "\"thread_group\",\"timestamp\",\"transaction\",\"verify\","
	    "\"version\",\"write\"]",
	    NULL, 0 },
	{ "version", "string", NULL, NULL, NULL, 0 },
	{ "write_through", "list",
	    NULL, "choices=[\"data\",\"log\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_basecfg[] = {
	{ "async", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_async_subconfigs, 3 },
	{ "buffer_alignment", "int", NULL, "min=-1,max=1MB", NULL, 0 },
	{ "builtin_extension_config", "string", NULL, NULL, NULL, 0 },
	{ "cache_cursors", "boolean", NULL, NULL, NULL, 0 },
	{ "cache_max_wait_ms", "int", NULL, "min=0", NULL, 0 },
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_checkpoint_subconfigs, 2 },
	{ "checkpoint_sync", "boolean", NULL, NULL, NULL, 0 },
	{ "compatibility", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_compatibility_subconfigs, 3 },
	{ "direct_io", "list",
	    NULL, "choices=[\"checkpoint\",\"data\",\"log\"]",
	    NULL, 0 },
	{ "encryption", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_encryption_subconfigs, 3 },
	{ "error_prefix", "string", NULL, NULL, NULL, 0 },
	{ "eviction", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_eviction_subconfigs, 2 },
	{ "eviction_checkpoint_target", "int",
	    NULL, "min=0,max=10TB",
	    NULL, 0 },
	{ "eviction_dirty_target", "int",
	    NULL, "min=1,max=10TB",
	    NULL, 0 },
	{ "eviction_dirty_trigger", "int",
	    NULL, "min=1,max=10TB",
	    NULL, 0 },
	{ "eviction_target", "int", NULL, "min=10,max=10TB", NULL, 0 },
	{ "eviction_trigger", "int",
	    NULL, "min=10,max=10TB",
	    NULL, 0 },
	{ "extensions", "list", NULL, NULL, NULL, 0 },
	{ "file_extend", "list",
	    NULL, "choices=[\"data\",\"log\"]",
	    NULL, 0 },
	{ "file_manager", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_file_manager_subconfigs, 3 },
	{ "hazard_max", "int", NULL, "min=15", NULL, 0 },
	{ "log", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_log_subconfigs, 8 },
	{ "lsm_manager", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_lsm_manager_subconfigs, 2 },
	{ "lsm_merge", "boolean", NULL, NULL, NULL, 0 },
	{ "mmap", "boolean", NULL, NULL, NULL, 0 },
	{ "multiprocess", "boolean", NULL, NULL, NULL, 0 },
	{ "operation_tracking", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_operation_tracking_subconfigs, 2 },
	{ "readonly", "boolean", NULL, NULL, NULL, 0 },
	{ "salvage", "boolean", NULL, NULL, NULL, 0 },
	{ "session_max", "int", NULL, "min=1", NULL, 0 },
	{ "session_scratch_max", "int", NULL, NULL, NULL, 0 },
	{ "session_table_cache", "boolean", NULL, NULL, NULL, 0 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_shared_cache_subconfigs, 5 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"cache_walk\",\"fast\",\"none\","
	    "\"clear\",\"tree_walk\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_statistics_log_subconfigs, 6 },
	{ "timing_stress_for_test", "list",
	    NULL, "choices=[\"checkpoint_slow\",\"lookaside_sweep_race\","
	    "\"split_1\",\"split_2\",\"split_3\",\"split_4\",\"split_5\","
	    "\"split_6\",\"split_7\",\"split_8\"]",
	    NULL, 0 },
	{ "transaction_sync", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_transaction_sync_subconfigs, 2 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\","
	    "\"checkpoint_progress\",\"compact\",\"error_returns\",\"evict\","
	    "\"evict_stuck\",\"evictserver\",\"fileops\",\"handleops\","
	    "\"log\",\"lookaside\",\"lookaside_activity\",\"lsm\","
	    "\"lsm_manager\",\"metadata\",\"mutex\",\"overflow\",\"read\","
	    "\"rebalance\",\"reconcile\",\"recovery\",\"recovery_progress\","
	    "\"salvage\",\"shared_cache\",\"split\",\"temporary\","
	    "\"thread_group\",\"timestamp\",\"transaction\",\"verify\","
	    "\"version\",\"write\"]",
	    NULL, 0 },
	{ "version", "string", NULL, NULL, NULL, 0 },
	{ "write_through", "list",
	    NULL, "choices=[\"data\",\"log\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_usercfg[] = {
	{ "async", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_async_subconfigs, 3 },
	{ "buffer_alignment", "int", NULL, "min=-1,max=1MB", NULL, 0 },
	{ "builtin_extension_config", "string", NULL, NULL, NULL, 0 },
	{ "cache_cursors", "boolean", NULL, NULL, NULL, 0 },
	{ "cache_max_wait_ms", "int", NULL, "min=0", NULL, 0 },
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_checkpoint_subconfigs, 2 },
	{ "checkpoint_sync", "boolean", NULL, NULL, NULL, 0 },
	{ "compatibility", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_compatibility_subconfigs, 3 },
	{ "direct_io", "list",
	    NULL, "choices=[\"checkpoint\",\"data\",\"log\"]",
	    NULL, 0 },
	{ "encryption", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_encryption_subconfigs, 3 },
	{ "error_prefix", "string", NULL, NULL, NULL, 0 },
	{ "eviction", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_eviction_subconfigs, 2 },
	{ "eviction_checkpoint_target", "int",
	    NULL, "min=0,max=10TB",
	    NULL, 0 },
	{ "eviction_dirty_target", "int",
	    NULL, "min=1,max=10TB",
	    NULL, 0 },
	{ "eviction_dirty_trigger", "int",
	    NULL, "min=1,max=10TB",
	    NULL, 0 },
	{ "eviction_target", "int", NULL, "min=10,max=10TB", NULL, 0 },
	{ "eviction_trigger", "int",
	    NULL, "min=10,max=10TB",
	    NULL, 0 },
	{ "extensions", "list", NULL, NULL, NULL, 0 },
	{ "file_extend", "list",
	    NULL, "choices=[\"data\",\"log\"]",
	    NULL, 0 },
	{ "file_manager", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_file_manager_subconfigs, 3 },
	{ "hazard_max", "int", NULL, "min=15", NULL, 0 },
	{ "log", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_log_subconfigs, 8 },
	{ "lsm_manager", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_lsm_manager_subconfigs, 2 },
	{ "lsm_merge", "boolean", NULL, NULL, NULL, 0 },
	{ "mmap", "boolean", NULL, NULL, NULL, 0 },
	{ "multiprocess", "boolean", NULL, NULL, NULL, 0 },
	{ "operation_tracking", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_operation_tracking_subconfigs, 2 },
	{ "readonly", "boolean", NULL, NULL, NULL, 0 },
	{ "salvage", "boolean", NULL, NULL, NULL, 0 },
	{ "session_max", "int", NULL, "min=1", NULL, 0 },
	{ "session_scratch_max", "int", NULL, NULL, NULL, 0 },
	{ "session_table_cache", "boolean", NULL, NULL, NULL, 0 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_shared_cache_subconfigs, 5 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"cache_walk\",\"fast\",\"none\","
	    "\"clear\",\"tree_walk\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_statistics_log_subconfigs, 6 },
	{ "timing_stress_for_test", "list",
	    NULL, "choices=[\"checkpoint_slow\",\"lookaside_sweep_race\","
	    "\"split_1\",\"split_2\",\"split_3\",\"split_4\",\"split_5\","
	    "\"split_6\",\"split_7\",\"split_8\"]",
	    NULL, 0 },
	{ "transaction_sync", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_transaction_sync_subconfigs, 2 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\","
	    "\"checkpoint_progress\",\"compact\",\"error_returns\",\"evict\","
	    "\"evict_stuck\",\"evictserver\",\"fileops\",\"handleops\","
	    "\"log\",\"lookaside\",\"lookaside_activity\",\"lsm\","
	    "\"lsm_manager\",\"metadata\",\"mutex\",\"overflow\",\"read\","
	    "\"rebalance\",\"reconcile\",\"recovery\",\"recovery_progress\","
	    "\"salvage\",\"shared_cache\",\"split\",\"temporary\","
	    "\"thread_group\",\"timestamp\",\"transaction\",\"verify\","
	    "\"version\",\"write\"]",
	    NULL, 0 },
	{ "write_through", "list",
	    NULL, "choices=[\"data\",\"log\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_ENTRY config_entries[] = {
	{ "WT_CONNECTION.add_collator",
	  "",
	  NULL, 0
	},
	{ "WT_CONNECTION.add_compressor",
	  "",
	  NULL, 0
	},
	{ "WT_CONNECTION.add_data_source",
	  "",
	  NULL, 0
	},
	{ "WT_CONNECTION.add_encryptor",
	  "",
	  NULL, 0
	},
	{ "WT_CONNECTION.add_extractor",
	  "",
	  NULL, 0
	},
	{ "WT_CONNECTION.async_new_op",
	  "append=false,overwrite=true,raw=false,timeout=1200",
	  confchk_WT_CONNECTION_async_new_op, 4
	},
	{ "WT_CONNECTION.close",
	  "leak_memory=false,use_timestamp=true",
	  confchk_WT_CONNECTION_close, 2
	},
	{ "WT_CONNECTION.debug_info",
	  "cache=false,cursors=false,handles=false,log=false,sessions=false"
	  ",txn=false",
	  confchk_WT_CONNECTION_debug_info, 6
	},
	{ "WT_CONNECTION.load_extension",
	  "config=,early_load=false,entry=wiredtiger_extension_init,"
	  "terminate=wiredtiger_extension_terminate",
	  confchk_WT_CONNECTION_load_extension, 4
	},
	{ "WT_CONNECTION.open_session",
	  "cache_cursors=true,ignore_cache_size=false,"
	  "isolation=read-committed",
	  confchk_WT_CONNECTION_open_session, 3
	},
	{ "WT_CONNECTION.query_timestamp",
	  "get=all_committed",
	  confchk_WT_CONNECTION_query_timestamp, 1
	},
	{ "WT_CONNECTION.reconfigure",
	  "async=(enabled=false,ops_max=1024,threads=2),cache_max_wait_ms=0"
	  ",cache_overhead=8,cache_size=100MB,checkpoint=(log_size=0,"
	  "wait=0),compatibility=(release=),error_prefix=,"
	  "eviction=(threads_max=8,threads_min=1),"
	  "eviction_checkpoint_target=1,eviction_dirty_target=5,"
	  "eviction_dirty_trigger=20,eviction_target=80,eviction_trigger=95"
	  ",file_manager=(close_handle_minimum=250,close_idle_time=30,"
	  "close_scan_interval=10),log=(archive=true,prealloc=true,"
	  "zero_fill=false),lsm_manager=(merge=true,worker_thread_max=4),"
	  "lsm_merge=true,operation_tracking=(enabled=false,path=\".\"),"
	  "shared_cache=(chunk=10MB,name=,quota=0,reserve=0,size=500MB),"
	  "statistics=none,statistics_log=(json=false,on_close=false,"
	  "sources=,timestamp=\"%b %d %H:%M:%S\",wait=0),"
	  "timing_stress_for_test=,verbose=",
	  confchk_WT_CONNECTION_reconfigure, 23
	},
	{ "WT_CONNECTION.rollback_to_stable",
	  "",
	  NULL, 0
	},
	{ "WT_CONNECTION.set_file_system",
	  "",
	  NULL, 0
	},
	{ "WT_CONNECTION.set_timestamp",
	  "commit_timestamp=,force=false,oldest_timestamp=,"
	  "stable_timestamp=",
	  confchk_WT_CONNECTION_set_timestamp, 4
	},
	{ "WT_CURSOR.close",
	  "",
	  NULL, 0
	},
	{ "WT_CURSOR.reconfigure",
	  "append=false,overwrite=true",
	  confchk_WT_CURSOR_reconfigure, 2
	},
	{ "WT_SESSION.alter",
	  "access_pattern_hint=none,app_metadata=,"
	  "assert=(commit_timestamp=none,read_timestamp=none),"
	  "cache_resident=false,exclusive_refreshed=true,log=(enabled=true)",
	  confchk_WT_SESSION_alter, 6
	},
	{ "WT_SESSION.begin_transaction",
	  "ignore_prepare=false,isolation=,name=,priority=0,read_timestamp="
	  ",round_to_oldest=false,snapshot=,sync=",
	  confchk_WT_SESSION_begin_transaction, 8
	},
	{ "WT_SESSION.checkpoint",
	  "drop=,force=false,name=,target=,use_timestamp=true",
	  confchk_WT_SESSION_checkpoint, 5
	},
	{ "WT_SESSION.close",
	  "",
	  NULL, 0
	},
	{ "WT_SESSION.commit_transaction",
	  "commit_timestamp=,sync=",
	  confchk_WT_SESSION_commit_transaction, 2
	},
	{ "WT_SESSION.compact",
	  "timeout=1200",
	  confchk_WT_SESSION_compact, 1
	},
	{ "WT_SESSION.create",
	  "access_pattern_hint=none,allocation_size=4KB,app_metadata=,"
	  "assert=(commit_timestamp=none,read_timestamp=none),"
	  "block_allocation=best,block_compressor=,cache_resident=false,"
	  "checksum=uncompressed,colgroups=,collator=,columns=,dictionary=0"
	  ",encryption=(keyid=,name=),exclusive=false,extractor=,"
	  "format=btree,huffman_key=,huffman_value=,"
	  "ignore_in_memory_cache_size=false,immutable=false,"
	  "internal_item_max=0,internal_key_max=0,"
	  "internal_key_truncate=true,internal_page_max=4KB,key_format=u,"
	  "key_gap=10,leaf_item_max=0,leaf_key_max=0,leaf_page_max=32KB,"
	  "leaf_value_max=0,log=(enabled=true),lsm=(auto_throttle=true,"
	  "bloom=true,bloom_bit_count=16,bloom_config=,bloom_hash_count=8,"
	  "bloom_oldest=false,chunk_count_limit=0,chunk_max=5GB,"
	  "chunk_size=10MB,merge_custom=(prefix=,start_generation=0,"
	  "suffix=),merge_max=15,merge_min=0),memory_page_image_max=0,"
	  "memory_page_max=5MB,os_cache_dirty_max=0,os_cache_max=0,"
	  "prefix_compression=false,prefix_compression_min=4,source=,"
	  "split_deepen_min_child=0,split_deepen_per_child=0,split_pct=90,"
	  "type=file,value_format=u",
	  confchk_WT_SESSION_create, 44
	},
	{ "WT_SESSION.drop",
	  "checkpoint_wait=true,force=false,lock_wait=true,"
	  "remove_files=true",
	  confchk_WT_SESSION_drop, 4
	},
	{ "WT_SESSION.join",
	  "bloom_bit_count=16,bloom_false_positives=false,"
	  "bloom_hash_count=8,compare=\"eq\",count=,operation=\"and\","
	  "strategy=",
	  confchk_WT_SESSION_join, 7
	},
	{ "WT_SESSION.log_flush",
	  "sync=on",
	  confchk_WT_SESSION_log_flush, 1
	},
	{ "WT_SESSION.log_printf",
	  "",
	  NULL, 0
	},
	{ "WT_SESSION.open_cursor",
	  "append=false,bulk=false,checkpoint=,checkpoint_wait=true,dump=,"
	  "next_random=false,next_random_sample_size=0,overwrite=true,"
	  "raw=false,read_once=false,readonly=false,skip_sort_check=false,"
	  "statistics=,target=",
	  confchk_WT_SESSION_open_cursor, 14
	},
	{ "WT_SESSION.prepare_transaction",
	  "prepare_timestamp=",
	  confchk_WT_SESSION_prepare_transaction, 1
	},
	{ "WT_SESSION.query_timestamp",
	  "get=read",
	  confchk_WT_SESSION_query_timestamp, 1
	},
	{ "WT_SESSION.rebalance",
	  "",
	  NULL, 0
	},
	{ "WT_SESSION.reconfigure",
	  "cache_cursors=true,ignore_cache_size=false,"
	  "isolation=read-committed",
	  confchk_WT_SESSION_reconfigure, 3
	},
	{ "WT_SESSION.rename",
	  "",
	  NULL, 0
	},
	{ "WT_SESSION.reset",
	  "",
	  NULL, 0
	},
	{ "WT_SESSION.rollback_transaction",
	  "",
	  NULL, 0
	},
	{ "WT_SESSION.salvage",
	  "force=false",
	  confchk_WT_SESSION_salvage, 1
	},
	{ "WT_SESSION.snapshot",
	  "drop=(all=false,before=,names=,to=),include_updates=false,name=",
	  confchk_WT_SESSION_snapshot, 3
	},
	{ "WT_SESSION.strerror",
	  "",
	  NULL, 0
	},
	{ "WT_SESSION.timestamp_transaction",
	  "commit_timestamp=,read_timestamp=,round_to_oldest=false",
	  confchk_WT_SESSION_timestamp_transaction, 3
	},
	{ "WT_SESSION.transaction_sync",
	  "timeout_ms=1200000",
	  confchk_WT_SESSION_transaction_sync, 1
	},
	{ "WT_SESSION.truncate",
	  "",
	  NULL, 0
	},
	{ "WT_SESSION.upgrade",
	  "",
	  NULL, 0
	},
	{ "WT_SESSION.verify",
	  "dump_address=false,dump_blocks=false,dump_layout=false,"
	  "dump_offsets=,dump_pages=false,strict=false",
	  confchk_WT_SESSION_verify, 6
	},
	{ "colgroup.meta",
	  "app_metadata=,collator=,columns=,source=,type=file",
	  confchk_colgroup_meta, 5
	},
	{ "file.config",
	  "access_pattern_hint=none,allocation_size=4KB,app_metadata=,"
	  "assert=(commit_timestamp=none,read_timestamp=none),"
	  "block_allocation=best,block_compressor=,cache_resident=false,"
	  "checksum=uncompressed,collator=,columns=,dictionary=0,"
	  "encryption=(keyid=,name=),format=btree,huffman_key=,"
	  "huffman_value=,ignore_in_memory_cache_size=false,"
	  "internal_item_max=0,internal_key_max=0,"
	  "internal_key_truncate=true,internal_page_max=4KB,key_format=u,"
	  "key_gap=10,leaf_item_max=0,leaf_key_max=0,leaf_page_max=32KB,"
	  "leaf_value_max=0,log=(enabled=true),memory_page_image_max=0,"
	  "memory_page_max=5MB,os_cache_dirty_max=0,os_cache_max=0,"
	  "prefix_compression=false,prefix_compression_min=4,"
	  "split_deepen_min_child=0,split_deepen_per_child=0,split_pct=90,"
	  "value_format=u",
	  confchk_file_config, 37
	},
	{ "file.meta",
	  "access_pattern_hint=none,allocation_size=4KB,app_metadata=,"
	  "assert=(commit_timestamp=none,read_timestamp=none),"
	  "block_allocation=best,block_compressor=,cache_resident=false,"
	  "checkpoint=,checkpoint_lsn=,checksum=uncompressed,collator=,"
	  "columns=,dictionary=0,encryption=(keyid=,name=),format=btree,"
	  "huffman_key=,huffman_value=,id=,"
	  "ignore_in_memory_cache_size=false,internal_item_max=0,"
	  "internal_key_max=0,internal_key_truncate=true,"
	  "internal_page_max=4KB,key_format=u,key_gap=10,leaf_item_max=0,"
	  "leaf_key_max=0,leaf_page_max=32KB,leaf_value_max=0,"
	  "log=(enabled=true),memory_page_image_max=0,memory_page_max=5MB,"
	  "os_cache_dirty_max=0,os_cache_max=0,prefix_compression=false,"
	  "prefix_compression_min=4,split_deepen_min_child=0,"
	  "split_deepen_per_child=0,split_pct=90,value_format=u,"
	  "version=(major=0,minor=0)",
	  confchk_file_meta, 41
	},
	{ "index.meta",
	  "app_metadata=,collator=,columns=,extractor=,immutable=false,"
	  "index_key_columns=,key_format=u,source=,type=file,value_format=u",
	  confchk_index_meta, 10
	},
	{ "lsm.meta",
	  "access_pattern_hint=none,allocation_size=4KB,app_metadata=,"
	  "assert=(commit_timestamp=none,read_timestamp=none),"
	  "block_allocation=best,block_compressor=,cache_resident=false,"
	  "checksum=uncompressed,chunks=,collator=,columns=,dictionary=0,"
	  "encryption=(keyid=,name=),format=btree,huffman_key=,"
	  "huffman_value=,ignore_in_memory_cache_size=false,"
	  "internal_item_max=0,internal_key_max=0,"
	  "internal_key_truncate=true,internal_page_max=4KB,key_format=u,"
	  "key_gap=10,last=,leaf_item_max=0,leaf_key_max=0,"
	  "leaf_page_max=32KB,leaf_value_max=0,log=(enabled=true),"
	  "lsm=(auto_throttle=true,bloom=true,bloom_bit_count=16,"
	  "bloom_config=,bloom_hash_count=8,bloom_oldest=false,"
	  "chunk_count_limit=0,chunk_max=5GB,chunk_size=10MB,"
	  "merge_custom=(prefix=,start_generation=0,suffix=),merge_max=15,"
	  "merge_min=0),memory_page_image_max=0,memory_page_max=5MB,"
	  "old_chunks=,os_cache_dirty_max=0,os_cache_max=0,"
	  "prefix_compression=false,prefix_compression_min=4,"
	  "split_deepen_min_child=0,split_deepen_per_child=0,split_pct=90,"
	  "value_format=u",
	  confchk_lsm_meta, 41
	},
	{ "table.meta",
	  "app_metadata=,colgroups=,collator=,columns=,key_format=u,"
	  "value_format=u",
	  confchk_table_meta, 6
	},
	{ "wiredtiger_open",
	  "async=(enabled=false,ops_max=1024,threads=2),buffer_alignment=-1"
	  ",builtin_extension_config=,cache_cursors=true,"
	  "cache_max_wait_ms=0,cache_overhead=8,cache_size=100MB,"
	  "checkpoint=(log_size=0,wait=0),checkpoint_sync=true,"
	  "compatibility=(release=,require_max=,require_min=),"
	  "config_base=true,create=false,direct_io=,encryption=(keyid=,"
	  "name=,secretkey=),error_prefix=,eviction=(threads_max=8,"
	  "threads_min=1),eviction_checkpoint_target=1,"
	  "eviction_dirty_target=5,eviction_dirty_trigger=20,"
	  "eviction_target=80,eviction_trigger=95,exclusive=false,"
	  "extensions=,file_extend=,file_manager=(close_handle_minimum=250,"
	  "close_idle_time=30,close_scan_interval=10),hazard_max=1000,"
	  "in_memory=false,log=(archive=true,compressor=,enabled=false,"
	  "file_max=100MB,path=\".\",prealloc=true,recover=on,"
	  "zero_fill=false),lsm_manager=(merge=true,worker_thread_max=4),"
	  "lsm_merge=true,mmap=true,multiprocess=false,"
	  "operation_tracking=(enabled=false,path=\".\"),readonly=false,"
	  "salvage=false,session_max=100,session_scratch_max=2MB,"
	  "session_table_cache=true,shared_cache=(chunk=10MB,name=,quota=0,"
	  "reserve=0,size=500MB),statistics=none,statistics_log=(json=false"
	  ",on_close=false,path=\".\",sources=,timestamp=\"%b %d %H:%M:%S\""
	  ",wait=0),timing_stress_for_test=,transaction_sync=(enabled=false"
	  ",method=fsync),use_environment=true,use_environment_priv=false,"
	  "verbose=,write_through=",
	  confchk_wiredtiger_open, 47
	},
	{ "wiredtiger_open_all",
	  "async=(enabled=false,ops_max=1024,threads=2),buffer_alignment=-1"
	  ",builtin_extension_config=,cache_cursors=true,"
	  "cache_max_wait_ms=0,cache_overhead=8,cache_size=100MB,"
	  "checkpoint=(log_size=0,wait=0),checkpoint_sync=true,"
	  "compatibility=(release=,require_max=,require_min=),"
	  "config_base=true,create=false,direct_io=,encryption=(keyid=,"
	  "name=,secretkey=),error_prefix=,eviction=(threads_max=8,"
	  "threads_min=1),eviction_checkpoint_target=1,"
	  "eviction_dirty_target=5,eviction_dirty_trigger=20,"
	  "eviction_target=80,eviction_trigger=95,exclusive=false,"
	  "extensions=,file_extend=,file_manager=(close_handle_minimum=250,"
	  "close_idle_time=30,close_scan_interval=10),hazard_max=1000,"
	  "in_memory=false,log=(archive=true,compressor=,enabled=false,"
	  "file_max=100MB,path=\".\",prealloc=true,recover=on,"
	  "zero_fill=false),lsm_manager=(merge=true,worker_thread_max=4),"
	  "lsm_merge=true,mmap=true,multiprocess=false,"
	  "operation_tracking=(enabled=false,path=\".\"),readonly=false,"
	  "salvage=false,session_max=100,session_scratch_max=2MB,"
	  "session_table_cache=true,shared_cache=(chunk=10MB,name=,quota=0,"
	  "reserve=0,size=500MB),statistics=none,statistics_log=(json=false"
	  ",on_close=false,path=\".\",sources=,timestamp=\"%b %d %H:%M:%S\""
	  ",wait=0),timing_stress_for_test=,transaction_sync=(enabled=false"
	  ",method=fsync),use_environment=true,use_environment_priv=false,"
	  "verbose=,version=(major=0,minor=0),write_through=",
	  confchk_wiredtiger_open_all, 48
	},
	{ "wiredtiger_open_basecfg",
	  "async=(enabled=false,ops_max=1024,threads=2),buffer_alignment=-1"
	  ",builtin_extension_config=,cache_cursors=true,"
	  "cache_max_wait_ms=0,cache_overhead=8,cache_size=100MB,"
	  "checkpoint=(log_size=0,wait=0),checkpoint_sync=true,"
	  "compatibility=(release=,require_max=,require_min=),direct_io=,"
	  "encryption=(keyid=,name=,secretkey=),error_prefix=,"
	  "eviction=(threads_max=8,threads_min=1),"
	  "eviction_checkpoint_target=1,eviction_dirty_target=5,"
	  "eviction_dirty_trigger=20,eviction_target=80,eviction_trigger=95"
	  ",extensions=,file_extend=,file_manager=(close_handle_minimum=250"
	  ",close_idle_time=30,close_scan_interval=10),hazard_max=1000,"
	  "log=(archive=true,compressor=,enabled=false,file_max=100MB,"
	  "path=\".\",prealloc=true,recover=on,zero_fill=false),"
	  "lsm_manager=(merge=true,worker_thread_max=4),lsm_merge=true,"
	  "mmap=true,multiprocess=false,operation_tracking=(enabled=false,"
	  "path=\".\"),readonly=false,salvage=false,session_max=100,"
	  "session_scratch_max=2MB,session_table_cache=true,"
	  "shared_cache=(chunk=10MB,name=,quota=0,reserve=0,size=500MB),"
	  "statistics=none,statistics_log=(json=false,on_close=false,"
	  "path=\".\",sources=,timestamp=\"%b %d %H:%M:%S\",wait=0),"
	  "timing_stress_for_test=,transaction_sync=(enabled=false,"
	  "method=fsync),verbose=,version=(major=0,minor=0),write_through=",
	  confchk_wiredtiger_open_basecfg, 42
	},
	{ "wiredtiger_open_usercfg",
	  "async=(enabled=false,ops_max=1024,threads=2),buffer_alignment=-1"
	  ",builtin_extension_config=,cache_cursors=true,"
	  "cache_max_wait_ms=0,cache_overhead=8,cache_size=100MB,"
	  "checkpoint=(log_size=0,wait=0),checkpoint_sync=true,"
	  "compatibility=(release=,require_max=,require_min=),direct_io=,"
	  "encryption=(keyid=,name=,secretkey=),error_prefix=,"
	  "eviction=(threads_max=8,threads_min=1),"
	  "eviction_checkpoint_target=1,eviction_dirty_target=5,"
	  "eviction_dirty_trigger=20,eviction_target=80,eviction_trigger=95"
	  ",extensions=,file_extend=,file_manager=(close_handle_minimum=250"
	  ",close_idle_time=30,close_scan_interval=10),hazard_max=1000,"
	  "log=(archive=true,compressor=,enabled=false,file_max=100MB,"
	  "path=\".\",prealloc=true,recover=on,zero_fill=false),"
	  "lsm_manager=(merge=true,worker_thread_max=4),lsm_merge=true,"
	  "mmap=true,multiprocess=false,operation_tracking=(enabled=false,"
	  "path=\".\"),readonly=false,salvage=false,session_max=100,"
	  "session_scratch_max=2MB,session_table_cache=true,"
	  "shared_cache=(chunk=10MB,name=,quota=0,reserve=0,size=500MB),"
	  "statistics=none,statistics_log=(json=false,on_close=false,"
	  "path=\".\",sources=,timestamp=\"%b %d %H:%M:%S\",wait=0),"
	  "timing_stress_for_test=,transaction_sync=(enabled=false,"
	  "method=fsync),verbose=,write_through=",
	  confchk_wiredtiger_open_usercfg, 41
	},
	{ NULL, NULL, NULL, 0 }
};

int
__wt_conn_config_init(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	const WT_CONFIG_ENTRY *ep, **epp;

	conn = S2C(session);

	/* Build a list of pointers to the configuration information. */
	WT_RET(__wt_calloc_def(session, WT_ELEMENTS(config_entries), &epp));
	conn->config_entries = epp;

	/* Fill in the list to reference the default information. */
	for (ep = config_entries;;) {
		*epp++ = ep++;
		if (ep->method == NULL)
			break;
	}
	return (0);
}

void
__wt_conn_config_discard(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	__wt_free(session, conn->config_entries);
}

/*
 * __wt_conn_config_match --
 *      Return the static configuration entry for a method.
 */
const WT_CONFIG_ENTRY *
__wt_conn_config_match(const char *method)
{
	const WT_CONFIG_ENTRY *ep;

	for (ep = config_entries; ep->method != NULL; ++ep)
		if (strcmp(method, ep->method) == 0)
			return (ep);
	return (NULL);
}
