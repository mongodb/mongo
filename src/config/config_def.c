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
	{ "isolation", "string",
	    NULL, "choices=[\"read-uncommitted\",\"read-committed\","
	    "\"snapshot\"]",
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
	{ "name", "string", NULL, NULL, NULL, 0 },
	{ "wait", "int", NULL, "min=0,max=100000", NULL, 0 },
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
    confchk_wiredtiger_open_lsm_manager_subconfigs[] = {
	{ "merge", "boolean", NULL, NULL, NULL, 0 },
	{ "worker_thread_max", "int", NULL, "min=3,max=20", NULL, 0 },
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
    confchk_wiredtiger_open_statistics_log_subconfigs[] = {
	{ "json", "boolean", NULL, NULL, NULL, 0 },
	{ "on_close", "boolean", NULL, NULL, NULL, 0 },
	{ "path", "string", NULL, NULL, NULL, 0 },
	{ "sources", "list", NULL, NULL, NULL, 0 },
	{ "timestamp", "string", NULL, NULL, NULL, 0 },
	{ "wait", "int", NULL, "min=0,max=100000", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_reconfigure[] = {
	{ "async", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_async_subconfigs, 3 },
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_checkpoint_subconfigs, 3 },
	{ "error_prefix", "string", NULL, NULL, NULL, 0 },
	{ "eviction", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_eviction_subconfigs, 2 },
	{ "eviction_dirty_target", "int",
	    NULL, "min=5,max=99",
	    NULL, 0 },
	{ "eviction_dirty_trigger", "int",
	    NULL, "min=5,max=99",
	    NULL, 0 },
	{ "eviction_target", "int", NULL, "min=10,max=99", NULL, 0 },
	{ "eviction_trigger", "int", NULL, "min=10,max=99", NULL, 0 },
	{ "file_manager", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_file_manager_subconfigs, 3 },
	{ "log", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_log_subconfigs, 8 },
	{ "lsm_manager", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_lsm_manager_subconfigs, 2 },
	{ "lsm_merge", "boolean", NULL, NULL, NULL, 0 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_shared_cache_subconfigs, 5 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_statistics_log_subconfigs, 6 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\",\"compact\","
	    "\"evict\",\"evictserver\",\"fileops\",\"handleops\",\"log\","
	    "\"lsm\",\"lsm_manager\",\"metadata\",\"mutex\",\"overflow\","
	    "\"read\",\"rebalance\",\"reconcile\",\"recovery\",\"salvage\","
	    "\"shared_cache\",\"split\",\"temporary\",\"transaction\","
	    "\"verify\",\"version\",\"write\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_CURSOR_reconfigure[] = {
	{ "append", "boolean", NULL, NULL, NULL, 0 },
	{ "overwrite", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_begin_transaction[] = {
	{ "isolation", "string",
	    NULL, "choices=[\"read-uncommitted\",\"read-committed\","
	    "\"snapshot\"]",
	    NULL, 0 },
	{ "name", "string", NULL, NULL, NULL, 0 },
	{ "priority", "int", NULL, "min=-100,max=100", NULL, 0 },
	{ "snapshot", "string", NULL, NULL, NULL, 0 },
	{ "sync", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_checkpoint[] = {
	{ "drop", "list", NULL, NULL, NULL, 0 },
	{ "force", "boolean", NULL, NULL, NULL, 0 },
	{ "name", "string", NULL, NULL, NULL, 0 },
	{ "target", "list", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_commit_transaction[] = {
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
    confchk_WT_SESSION_create_log_subconfigs[] = {
	{ "enabled", "boolean", NULL, NULL, NULL, 0 },
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
	{ "merge_max", "int", NULL, "min=2,max=100", NULL, 0 },
	{ "merge_min", "int", NULL, "max=100", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_create[] = {
	{ "allocation_size", "int",
	    NULL, "min=512B,max=128MB",
	    NULL, 0 },
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
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
	    confchk_WT_SESSION_create_lsm_subconfigs, 11 },
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
	{ "split_pct", "int", NULL, "min=25,max=100", NULL, 0 },
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
	{ "dump", "string",
	    NULL, "choices=[\"hex\",\"json\",\"print\"]",
	    NULL, 0 },
	{ "next_random", "boolean", NULL, NULL, NULL, 0 },
	{ "next_random_sample_size", "string", NULL, NULL, NULL, 0 },
	{ "overwrite", "boolean", NULL, NULL, NULL, 0 },
	{ "raw", "boolean", NULL, NULL, NULL, 0 },
	{ "readonly", "boolean", NULL, NULL, NULL, 0 },
	{ "skip_sort_check", "boolean", NULL, NULL, NULL, 0 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"fast\",\"clear\",\"size\"]",
	    NULL, 0 },
	{ "target", "list", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_WT_SESSION_reconfigure[] = {
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
	{ "name", "string", NULL, NULL, NULL, 0 },
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
	{ "allocation_size", "int",
	    NULL, "min=512B,max=128MB",
	    NULL, 0 },
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
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
	{ "memory_page_max", "int",
	    NULL, "min=512B,max=10TB",
	    NULL, 0 },
	{ "os_cache_dirty_max", "int", NULL, "min=0", NULL, 0 },
	{ "os_cache_max", "int", NULL, "min=0", NULL, 0 },
	{ "prefix_compression", "boolean", NULL, NULL, NULL, 0 },
	{ "prefix_compression_min", "int", NULL, "min=0", NULL, 0 },
	{ "split_deepen_min_child", "int", NULL, NULL, NULL, 0 },
	{ "split_deepen_per_child", "int", NULL, NULL, NULL, 0 },
	{ "split_pct", "int", NULL, "min=25,max=100", NULL, 0 },
	{ "value_format", "format",
	    __wt_struct_confchk, NULL,
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_file_meta[] = {
	{ "allocation_size", "int",
	    NULL, "min=512B,max=128MB",
	    NULL, 0 },
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
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
	{ "memory_page_max", "int",
	    NULL, "min=512B,max=10TB",
	    NULL, 0 },
	{ "os_cache_dirty_max", "int", NULL, "min=0", NULL, 0 },
	{ "os_cache_max", "int", NULL, "min=0", NULL, 0 },
	{ "prefix_compression", "boolean", NULL, NULL, NULL, 0 },
	{ "prefix_compression_min", "int", NULL, "min=0", NULL, 0 },
	{ "split_deepen_min_child", "int", NULL, NULL, NULL, 0 },
	{ "split_deepen_per_child", "int", NULL, NULL, NULL, 0 },
	{ "split_pct", "int", NULL, "min=25,max=100", NULL, 0 },
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
	{ "allocation_size", "int",
	    NULL, "min=512B,max=128MB",
	    NULL, 0 },
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
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
	    confchk_WT_SESSION_create_lsm_subconfigs, 11 },
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
	{ "split_pct", "int", NULL, "min=25,max=100", NULL, 0 },
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
    confchk_wiredtiger_open_encryption_subconfigs[] = {
	{ "keyid", "string", NULL, NULL, NULL, 0 },
	{ "name", "string", NULL, NULL, NULL, 0 },
	{ "secretkey", "string", NULL, NULL, NULL, 0 },
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
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_checkpoint_subconfigs, 3 },
	{ "checkpoint_sync", "boolean", NULL, NULL, NULL, 0 },
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
	{ "eviction_dirty_target", "int",
	    NULL, "min=5,max=99",
	    NULL, 0 },
	{ "eviction_dirty_trigger", "int",
	    NULL, "min=5,max=99",
	    NULL, 0 },
	{ "eviction_target", "int", NULL, "min=10,max=99", NULL, 0 },
	{ "eviction_trigger", "int", NULL, "min=10,max=99", NULL, 0 },
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
	{ "readonly", "boolean", NULL, NULL, NULL, 0 },
	{ "session_max", "int", NULL, "min=1", NULL, 0 },
	{ "session_scratch_max", "int", NULL, NULL, NULL, 0 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_shared_cache_subconfigs, 5 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_statistics_log_subconfigs, 6 },
	{ "transaction_sync", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_transaction_sync_subconfigs, 2 },
	{ "use_environment", "boolean", NULL, NULL, NULL, 0 },
	{ "use_environment_priv", "boolean", NULL, NULL, NULL, 0 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\",\"compact\","
	    "\"evict\",\"evictserver\",\"fileops\",\"handleops\",\"log\","
	    "\"lsm\",\"lsm_manager\",\"metadata\",\"mutex\",\"overflow\","
	    "\"read\",\"rebalance\",\"reconcile\",\"recovery\",\"salvage\","
	    "\"shared_cache\",\"split\",\"temporary\",\"transaction\","
	    "\"verify\",\"version\",\"write\"]",
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
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_checkpoint_subconfigs, 3 },
	{ "checkpoint_sync", "boolean", NULL, NULL, NULL, 0 },
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
	{ "eviction_dirty_target", "int",
	    NULL, "min=5,max=99",
	    NULL, 0 },
	{ "eviction_dirty_trigger", "int",
	    NULL, "min=5,max=99",
	    NULL, 0 },
	{ "eviction_target", "int", NULL, "min=10,max=99", NULL, 0 },
	{ "eviction_trigger", "int", NULL, "min=10,max=99", NULL, 0 },
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
	{ "readonly", "boolean", NULL, NULL, NULL, 0 },
	{ "session_max", "int", NULL, "min=1", NULL, 0 },
	{ "session_scratch_max", "int", NULL, NULL, NULL, 0 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_shared_cache_subconfigs, 5 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_statistics_log_subconfigs, 6 },
	{ "transaction_sync", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_transaction_sync_subconfigs, 2 },
	{ "use_environment", "boolean", NULL, NULL, NULL, 0 },
	{ "use_environment_priv", "boolean", NULL, NULL, NULL, 0 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\",\"compact\","
	    "\"evict\",\"evictserver\",\"fileops\",\"handleops\",\"log\","
	    "\"lsm\",\"lsm_manager\",\"metadata\",\"mutex\",\"overflow\","
	    "\"read\",\"rebalance\",\"reconcile\",\"recovery\",\"salvage\","
	    "\"shared_cache\",\"split\",\"temporary\",\"transaction\","
	    "\"verify\",\"version\",\"write\"]",
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
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_checkpoint_subconfigs, 3 },
	{ "checkpoint_sync", "boolean", NULL, NULL, NULL, 0 },
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
	{ "eviction_dirty_target", "int",
	    NULL, "min=5,max=99",
	    NULL, 0 },
	{ "eviction_dirty_trigger", "int",
	    NULL, "min=5,max=99",
	    NULL, 0 },
	{ "eviction_target", "int", NULL, "min=10,max=99", NULL, 0 },
	{ "eviction_trigger", "int", NULL, "min=10,max=99", NULL, 0 },
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
	{ "readonly", "boolean", NULL, NULL, NULL, 0 },
	{ "session_max", "int", NULL, "min=1", NULL, 0 },
	{ "session_scratch_max", "int", NULL, NULL, NULL, 0 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_shared_cache_subconfigs, 5 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_statistics_log_subconfigs, 6 },
	{ "transaction_sync", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_transaction_sync_subconfigs, 2 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\",\"compact\","
	    "\"evict\",\"evictserver\",\"fileops\",\"handleops\",\"log\","
	    "\"lsm\",\"lsm_manager\",\"metadata\",\"mutex\",\"overflow\","
	    "\"read\",\"rebalance\",\"reconcile\",\"recovery\",\"salvage\","
	    "\"shared_cache\",\"split\",\"temporary\",\"transaction\","
	    "\"verify\",\"version\",\"write\"]",
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
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_checkpoint_subconfigs, 3 },
	{ "checkpoint_sync", "boolean", NULL, NULL, NULL, 0 },
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
	{ "eviction_dirty_target", "int",
	    NULL, "min=5,max=99",
	    NULL, 0 },
	{ "eviction_dirty_trigger", "int",
	    NULL, "min=5,max=99",
	    NULL, 0 },
	{ "eviction_target", "int", NULL, "min=10,max=99", NULL, 0 },
	{ "eviction_trigger", "int", NULL, "min=10,max=99", NULL, 0 },
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
	{ "readonly", "boolean", NULL, NULL, NULL, 0 },
	{ "session_max", "int", NULL, "min=1", NULL, 0 },
	{ "session_scratch_max", "int", NULL, NULL, NULL, 0 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_shared_cache_subconfigs, 5 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_statistics_log_subconfigs, 6 },
	{ "transaction_sync", "category",
	    NULL, NULL,
	    confchk_wiredtiger_open_transaction_sync_subconfigs, 2 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\",\"compact\","
	    "\"evict\",\"evictserver\",\"fileops\",\"handleops\",\"log\","
	    "\"lsm\",\"lsm_manager\",\"metadata\",\"mutex\",\"overflow\","
	    "\"read\",\"rebalance\",\"reconcile\",\"recovery\",\"salvage\","
	    "\"shared_cache\",\"split\",\"temporary\",\"transaction\","
	    "\"verify\",\"version\",\"write\"]",
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
	  "append=0,overwrite=,raw=0,timeout=1200",
	  confchk_WT_CONNECTION_async_new_op, 4
	},
	{ "WT_CONNECTION.close",
	  "leak_memory=0",
	  confchk_WT_CONNECTION_close, 1
	},
	{ "WT_CONNECTION.load_extension",
	  "config=,early_load=0,entry=wiredtiger_extension_init,"
	  "terminate=wiredtiger_extension_terminate",
	  confchk_WT_CONNECTION_load_extension, 4
	},
	{ "WT_CONNECTION.open_session",
	  "isolation=read-committed",
	  confchk_WT_CONNECTION_open_session, 1
	},
	{ "WT_CONNECTION.reconfigure",
	  "async=(enabled=0,ops_max=1024,threads=2),cache_overhead=8,"
	  "cache_size=100MB,checkpoint=(log_size=0,"
	  "name=\"WiredTigerCheckpoint\",wait=0),error_prefix=,"
	  "eviction=(threads_max=1,threads_min=1),eviction_dirty_target=80,"
	  "eviction_dirty_trigger=95,eviction_target=80,eviction_trigger=95"
	  ",file_manager=(close_handle_minimum=250,close_idle_time=30,"
	  "close_scan_interval=10),log=(archive=,compressor=,enabled=0,"
	  "file_max=100MB,path=\".\",prealloc=,recover=on,zero_fill=0),"
	  "lsm_manager=(merge=,worker_thread_max=4),lsm_merge=,"
	  "shared_cache=(chunk=10MB,name=,quota=0,reserve=0,size=500MB),"
	  "statistics=none,statistics_log=(json=0,on_close=0,"
	  "path=\"WiredTigerStat.%d.%H\",sources=,"
	  "timestamp=\"%b %d %H:%M:%S\",wait=0),verbose=",
	  confchk_WT_CONNECTION_reconfigure, 18
	},
	{ "WT_CONNECTION.set_file_system",
	  "",
	  NULL, 0
	},
	{ "WT_CURSOR.close",
	  "",
	  NULL, 0
	},
	{ "WT_CURSOR.reconfigure",
	  "append=0,overwrite=",
	  confchk_WT_CURSOR_reconfigure, 2
	},
	{ "WT_SESSION.begin_transaction",
	  "isolation=,name=,priority=0,snapshot=,sync=",
	  confchk_WT_SESSION_begin_transaction, 5
	},
	{ "WT_SESSION.checkpoint",
	  "drop=,force=0,name=,target=",
	  confchk_WT_SESSION_checkpoint, 4
	},
	{ "WT_SESSION.close",
	  "",
	  NULL, 0
	},
	{ "WT_SESSION.commit_transaction",
	  "sync=",
	  confchk_WT_SESSION_commit_transaction, 1
	},
	{ "WT_SESSION.compact",
	  "timeout=1200",
	  confchk_WT_SESSION_compact, 1
	},
	{ "WT_SESSION.create",
	  "allocation_size=4KB,app_metadata=,block_allocation=best,"
	  "block_compressor=,cache_resident=0,checksum=uncompressed,"
	  "colgroups=,collator=,columns=,dictionary=0,encryption=(keyid=,"
	  "name=),exclusive=0,extractor=,format=btree,huffman_key=,"
	  "huffman_value=,immutable=0,internal_item_max=0,"
	  "internal_key_max=0,internal_key_truncate=,internal_page_max=4KB,"
	  "key_format=u,key_gap=10,leaf_item_max=0,leaf_key_max=0,"
	  "leaf_page_max=32KB,leaf_value_max=0,log=(enabled=),"
	  "lsm=(auto_throttle=,bloom=,bloom_bit_count=16,bloom_config=,"
	  "bloom_hash_count=8,bloom_oldest=0,chunk_count_limit=0,"
	  "chunk_max=5GB,chunk_size=10MB,merge_max=15,merge_min=0),"
	  "memory_page_max=5MB,os_cache_dirty_max=0,os_cache_max=0,"
	  "prefix_compression=0,prefix_compression_min=4,source=,"
	  "split_deepen_min_child=0,split_deepen_per_child=0,split_pct=75,"
	  "type=file,value_format=u",
	  confchk_WT_SESSION_create, 40
	},
	{ "WT_SESSION.drop",
	  "checkpoint_wait=,force=0,lock_wait=,remove_files=",
	  confchk_WT_SESSION_drop, 4
	},
	{ "WT_SESSION.join",
	  "bloom_bit_count=16,bloom_hash_count=8,compare=\"eq\",count=,"
	  "operation=\"and\",strategy=",
	  confchk_WT_SESSION_join, 6
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
	  "append=0,bulk=0,checkpoint=,dump=,next_random=0,"
	  "next_random_sample_size=0,overwrite=,raw=0,readonly=0,"
	  "skip_sort_check=0,statistics=,target=",
	  confchk_WT_SESSION_open_cursor, 12
	},
	{ "WT_SESSION.rebalance",
	  "",
	  NULL, 0
	},
	{ "WT_SESSION.reconfigure",
	  "isolation=read-committed",
	  confchk_WT_SESSION_reconfigure, 1
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
	  "force=0",
	  confchk_WT_SESSION_salvage, 1
	},
	{ "WT_SESSION.snapshot",
	  "drop=(all=0,before=,names=,to=),name=",
	  confchk_WT_SESSION_snapshot, 2
	},
	{ "WT_SESSION.strerror",
	  "",
	  NULL, 0
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
	  "dump_address=0,dump_blocks=0,dump_layout=0,dump_offsets=,"
	  "dump_pages=0,strict=0",
	  confchk_WT_SESSION_verify, 6
	},
	{ "colgroup.meta",
	  "app_metadata=,collator=,columns=,source=,type=file",
	  confchk_colgroup_meta, 5
	},
	{ "file.config",
	  "allocation_size=4KB,app_metadata=,block_allocation=best,"
	  "block_compressor=,cache_resident=0,checksum=uncompressed,"
	  "collator=,columns=,dictionary=0,encryption=(keyid=,name=),"
	  "format=btree,huffman_key=,huffman_value=,internal_item_max=0,"
	  "internal_key_max=0,internal_key_truncate=,internal_page_max=4KB,"
	  "key_format=u,key_gap=10,leaf_item_max=0,leaf_key_max=0,"
	  "leaf_page_max=32KB,leaf_value_max=0,log=(enabled=),"
	  "memory_page_max=5MB,os_cache_dirty_max=0,os_cache_max=0,"
	  "prefix_compression=0,prefix_compression_min=4,"
	  "split_deepen_min_child=0,split_deepen_per_child=0,split_pct=75,"
	  "value_format=u",
	  confchk_file_config, 33
	},
	{ "file.meta",
	  "allocation_size=4KB,app_metadata=,block_allocation=best,"
	  "block_compressor=,cache_resident=0,checkpoint=,checkpoint_lsn=,"
	  "checksum=uncompressed,collator=,columns=,dictionary=0,"
	  "encryption=(keyid=,name=),format=btree,huffman_key=,"
	  "huffman_value=,id=,internal_item_max=0,internal_key_max=0,"
	  "internal_key_truncate=,internal_page_max=4KB,key_format=u,"
	  "key_gap=10,leaf_item_max=0,leaf_key_max=0,leaf_page_max=32KB,"
	  "leaf_value_max=0,log=(enabled=),memory_page_max=5MB,"
	  "os_cache_dirty_max=0,os_cache_max=0,prefix_compression=0,"
	  "prefix_compression_min=4,split_deepen_min_child=0,"
	  "split_deepen_per_child=0,split_pct=75,value_format=u,"
	  "version=(major=0,minor=0)",
	  confchk_file_meta, 37
	},
	{ "index.meta",
	  "app_metadata=,collator=,columns=,extractor=,immutable=0,"
	  "index_key_columns=,key_format=u,source=,type=file,value_format=u",
	  confchk_index_meta, 10
	},
	{ "lsm.meta",
	  "allocation_size=4KB,app_metadata=,block_allocation=best,"
	  "block_compressor=,cache_resident=0,checksum=uncompressed,chunks="
	  ",collator=,columns=,dictionary=0,encryption=(keyid=,name=),"
	  "format=btree,huffman_key=,huffman_value=,internal_item_max=0,"
	  "internal_key_max=0,internal_key_truncate=,internal_page_max=4KB,"
	  "key_format=u,key_gap=10,last=,leaf_item_max=0,leaf_key_max=0,"
	  "leaf_page_max=32KB,leaf_value_max=0,log=(enabled=),"
	  "lsm=(auto_throttle=,bloom=,bloom_bit_count=16,bloom_config=,"
	  "bloom_hash_count=8,bloom_oldest=0,chunk_count_limit=0,"
	  "chunk_max=5GB,chunk_size=10MB,merge_max=15,merge_min=0),"
	  "memory_page_max=5MB,old_chunks=,os_cache_dirty_max=0,"
	  "os_cache_max=0,prefix_compression=0,prefix_compression_min=4,"
	  "split_deepen_min_child=0,split_deepen_per_child=0,split_pct=75,"
	  "value_format=u",
	  confchk_lsm_meta, 37
	},
	{ "table.meta",
	  "app_metadata=,colgroups=,collator=,columns=,key_format=u,"
	  "value_format=u",
	  confchk_table_meta, 6
	},
	{ "wiredtiger_open",
	  "async=(enabled=0,ops_max=1024,threads=2),buffer_alignment=-1,"
	  "cache_overhead=8,cache_size=100MB,checkpoint=(log_size=0,"
	  "name=\"WiredTigerCheckpoint\",wait=0),checkpoint_sync=,"
	  "config_base=,create=0,direct_io=,encryption=(keyid=,name=,"
	  "secretkey=),error_prefix=,eviction=(threads_max=1,threads_min=1)"
	  ",eviction_dirty_target=80,eviction_dirty_trigger=95,"
	  "eviction_target=80,eviction_trigger=95,exclusive=0,extensions=,"
	  "file_extend=,file_manager=(close_handle_minimum=250,"
	  "close_idle_time=30,close_scan_interval=10),hazard_max=1000,"
	  "in_memory=0,log=(archive=,compressor=,enabled=0,file_max=100MB,"
	  "path=\".\",prealloc=,recover=on,zero_fill=0),lsm_manager=(merge="
	  ",worker_thread_max=4),lsm_merge=,mmap=,multiprocess=0,readonly=0"
	  ",session_max=100,session_scratch_max=2MB,"
	  "shared_cache=(chunk=10MB,name=,quota=0,reserve=0,size=500MB),"
	  "statistics=none,statistics_log=(json=0,on_close=0,"
	  "path=\"WiredTigerStat.%d.%H\",sources=,"
	  "timestamp=\"%b %d %H:%M:%S\",wait=0),transaction_sync=(enabled=0"
	  ",method=fsync),use_environment=,use_environment_priv=0,verbose=,"
	  "write_through=",
	  confchk_wiredtiger_open, 38
	},
	{ "wiredtiger_open_all",
	  "async=(enabled=0,ops_max=1024,threads=2),buffer_alignment=-1,"
	  "cache_overhead=8,cache_size=100MB,checkpoint=(log_size=0,"
	  "name=\"WiredTigerCheckpoint\",wait=0),checkpoint_sync=,"
	  "config_base=,create=0,direct_io=,encryption=(keyid=,name=,"
	  "secretkey=),error_prefix=,eviction=(threads_max=1,threads_min=1)"
	  ",eviction_dirty_target=80,eviction_dirty_trigger=95,"
	  "eviction_target=80,eviction_trigger=95,exclusive=0,extensions=,"
	  "file_extend=,file_manager=(close_handle_minimum=250,"
	  "close_idle_time=30,close_scan_interval=10),hazard_max=1000,"
	  "in_memory=0,log=(archive=,compressor=,enabled=0,file_max=100MB,"
	  "path=\".\",prealloc=,recover=on,zero_fill=0),lsm_manager=(merge="
	  ",worker_thread_max=4),lsm_merge=,mmap=,multiprocess=0,readonly=0"
	  ",session_max=100,session_scratch_max=2MB,"
	  "shared_cache=(chunk=10MB,name=,quota=0,reserve=0,size=500MB),"
	  "statistics=none,statistics_log=(json=0,on_close=0,"
	  "path=\"WiredTigerStat.%d.%H\",sources=,"
	  "timestamp=\"%b %d %H:%M:%S\",wait=0),transaction_sync=(enabled=0"
	  ",method=fsync),use_environment=,use_environment_priv=0,verbose=,"
	  "version=(major=0,minor=0),write_through=",
	  confchk_wiredtiger_open_all, 39
	},
	{ "wiredtiger_open_basecfg",
	  "async=(enabled=0,ops_max=1024,threads=2),buffer_alignment=-1,"
	  "cache_overhead=8,cache_size=100MB,checkpoint=(log_size=0,"
	  "name=\"WiredTigerCheckpoint\",wait=0),checkpoint_sync=,"
	  "direct_io=,encryption=(keyid=,name=,secretkey=),error_prefix=,"
	  "eviction=(threads_max=1,threads_min=1),eviction_dirty_target=80,"
	  "eviction_dirty_trigger=95,eviction_target=80,eviction_trigger=95"
	  ",extensions=,file_extend=,file_manager=(close_handle_minimum=250"
	  ",close_idle_time=30,close_scan_interval=10),hazard_max=1000,"
	  "log=(archive=,compressor=,enabled=0,file_max=100MB,path=\".\","
	  "prealloc=,recover=on,zero_fill=0),lsm_manager=(merge=,"
	  "worker_thread_max=4),lsm_merge=,mmap=,multiprocess=0,readonly=0,"
	  "session_max=100,session_scratch_max=2MB,shared_cache=(chunk=10MB"
	  ",name=,quota=0,reserve=0,size=500MB),statistics=none,"
	  "statistics_log=(json=0,on_close=0,path=\"WiredTigerStat.%d.%H\","
	  "sources=,timestamp=\"%b %d %H:%M:%S\",wait=0),"
	  "transaction_sync=(enabled=0,method=fsync),verbose=,"
	  "version=(major=0,minor=0),write_through=",
	  confchk_wiredtiger_open_basecfg, 33
	},
	{ "wiredtiger_open_usercfg",
	  "async=(enabled=0,ops_max=1024,threads=2),buffer_alignment=-1,"
	  "cache_overhead=8,cache_size=100MB,checkpoint=(log_size=0,"
	  "name=\"WiredTigerCheckpoint\",wait=0),checkpoint_sync=,"
	  "direct_io=,encryption=(keyid=,name=,secretkey=),error_prefix=,"
	  "eviction=(threads_max=1,threads_min=1),eviction_dirty_target=80,"
	  "eviction_dirty_trigger=95,eviction_target=80,eviction_trigger=95"
	  ",extensions=,file_extend=,file_manager=(close_handle_minimum=250"
	  ",close_idle_time=30,close_scan_interval=10),hazard_max=1000,"
	  "log=(archive=,compressor=,enabled=0,file_max=100MB,path=\".\","
	  "prealloc=,recover=on,zero_fill=0),lsm_manager=(merge=,"
	  "worker_thread_max=4),lsm_merge=,mmap=,multiprocess=0,readonly=0,"
	  "session_max=100,session_scratch_max=2MB,shared_cache=(chunk=10MB"
	  ",name=,quota=0,reserve=0,size=500MB),statistics=none,"
	  "statistics_log=(json=0,on_close=0,path=\"WiredTigerStat.%d.%H\","
	  "sources=,timestamp=\"%b %d %H:%M:%S\",wait=0),"
	  "transaction_sync=(enabled=0,method=fsync),verbose=,"
	  "write_through=",
	  confchk_wiredtiger_open_usercfg, 32
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
