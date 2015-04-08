/* DO NOT EDIT: automatically built by dist/api_config.py. */

#include "wt_internal.h"

static const WT_CONFIG_CHECK confchk_colgroup_meta[] = {
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
	{ "collator", "string", __wt_collator_confchk, NULL, NULL, 0 },
	{ "columns", "list", NULL, NULL, NULL, 0 },
	{ "source", "string", NULL, NULL, NULL, 0 },
	{ "type", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_connection_async_new_op[] = {
	{ "append", "boolean", NULL, NULL, NULL, 0 },
	{ "overwrite", "boolean", NULL, NULL, NULL, 0 },
	{ "raw", "boolean", NULL, NULL, NULL, 0 },
	{ "timeout", "int", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_connection_close[] = {
	{ "leak_memory", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_connection_load_extension[] = {
	{ "config", "string", NULL, NULL, NULL, 0 },
	{ "entry", "string", NULL, NULL, NULL, 0 },
	{ "terminate", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_connection_open_session[] = {
	{ "isolation", "string",
	    NULL, "choices=[\"read-uncommitted\",\"read-committed\","
	    "\"snapshot\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_async_subconfigs[] = {
	{ "enabled", "boolean", NULL, NULL, NULL, 0 },
	{ "ops_max", "int", NULL, "min=1,max=4096", NULL, 0 },
	{ "threads", "int", NULL, "min=1,max=20", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_checkpoint_subconfigs[] = {
	{ "log_size", "int", NULL, "min=0,max=2GB", NULL, 0 },
	{ "name", "string", NULL, NULL, NULL, 0 },
	{ "wait", "int", NULL, "min=0,max=100000", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_eviction_subconfigs[] = {
	{ "threads_max", "int", NULL, "min=1,max=20", NULL, 0 },
	{ "threads_min", "int", NULL, "min=1,max=20", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_file_manager_subconfigs[] = {
	{ "close_idle_time", "int", NULL, "min=1,max=1000", NULL, 0 },
	{ "close_scan_interval", "int",
	    NULL, "min=1,max=1000",
	    NULL, 0 },
	{ "open_handles", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_lsm_manager_subconfigs[] = {
	{ "merge", "boolean", NULL, NULL, NULL, 0 },
	{ "worker_thread_max", "int", NULL, "min=3,max=20", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_shared_cache_subconfigs[] = {
	{ "chunk", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "name", "string", NULL, NULL, NULL, 0 },
	{ "reserve", "int", NULL, NULL, NULL, 0 },
	{ "size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_statistics_log_subconfigs[] = {
	{ "on_close", "boolean", NULL, NULL, NULL, 0 },
	{ "path", "string", NULL, NULL, NULL, 0 },
	{ "sources", "list", NULL, NULL, NULL, 0 },
	{ "timestamp", "string", NULL, NULL, NULL, 0 },
	{ "wait", "int", NULL, "min=0,max=100000", NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_connection_reconfigure[] = {
	{ "async", "category",
	    NULL, NULL,
	    confchk_async_subconfigs, 3 },
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_checkpoint_subconfigs, 3 },
	{ "error_prefix", "string", NULL, NULL, NULL, 0 },
	{ "eviction", "category",
	    NULL, NULL,
	    confchk_eviction_subconfigs, 2 },
	{ "eviction_dirty_target", "int",
	    NULL, "min=10,max=99",
	    NULL, 0 },
	{ "eviction_target", "int", NULL, "min=10,max=99", NULL, 0 },
	{ "eviction_trigger", "int", NULL, "min=10,max=99", NULL, 0 },
	{ "file_manager", "category",
	    NULL, NULL,
	    confchk_file_manager_subconfigs, 3 },
	{ "lsm_manager", "category",
	    NULL, NULL,
	    confchk_lsm_manager_subconfigs, 2 },
	{ "lsm_merge", "boolean", NULL, NULL, NULL, 0 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_shared_cache_subconfigs, 4 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_statistics_log_subconfigs, 5 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\",\"compact\","
	    "\"evict\",\"evictserver\",\"fileops\",\"log\",\"lsm\","
	    "\"metadata\",\"mutex\",\"overflow\",\"read\",\"reconcile\","
	    "\"recovery\",\"salvage\",\"shared_cache\",\"split\","
	    "\"temporary\",\"transaction\",\"verify\",\"version\",\"write\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_cursor_reconfigure[] = {
	{ "append", "boolean", NULL, NULL, NULL, 0 },
	{ "overwrite", "boolean", NULL, NULL, NULL, 0 },
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
	{ "block_compressor", "string",
	    __wt_compressor_confchk, NULL,
	    NULL, 0 },
	{ "cache_resident", "boolean", NULL, NULL, NULL, 0 },
	{ "checkpoint", "string", NULL, NULL, NULL, 0 },
	{ "checkpoint_lsn", "string", NULL, NULL, NULL, 0 },
	{ "checksum", "string",
	    NULL, "choices=[\"on\",\"off\",\"uncompressed\"]",
	    NULL, 0 },
	{ "collator", "string", __wt_collator_confchk, NULL, NULL, 0 },
	{ "columns", "list", NULL, NULL, NULL, 0 },
	{ "dictionary", "int", NULL, "min=0", NULL, 0 },
	{ "format", "string", NULL, "choices=[\"btree\"]", NULL, 0 },
	{ "huffman_key", "string",
	    __wt_huffman_confchk, NULL,
	    NULL, 0 },
	{ "huffman_value", "string",
	    __wt_huffman_confchk, NULL,
	    NULL, 0 },
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
	{ "collator", "string", __wt_collator_confchk, NULL, NULL, 0 },
	{ "columns", "list", NULL, NULL, NULL, 0 },
	{ "extractor", "string",
	    __wt_extractor_confchk, NULL,
	    NULL, 0 },
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

static const WT_CONFIG_CHECK confchk_session_begin_transaction[] = {
	{ "isolation", "string",
	    NULL, "choices=[\"read-uncommitted\",\"read-committed\","
	    "\"snapshot\"]",
	    NULL, 0 },
	{ "name", "string", NULL, NULL, NULL, 0 },
	{ "priority", "int", NULL, "min=-100,max=100", NULL, 0 },
	{ "sync", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_session_checkpoint[] = {
	{ "drop", "list", NULL, NULL, NULL, 0 },
	{ "force", "boolean", NULL, NULL, NULL, 0 },
	{ "name", "string", NULL, NULL, NULL, 0 },
	{ "target", "list", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_session_compact[] = {
	{ "timeout", "int", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_lsm_subconfigs[] = {
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

static const WT_CONFIG_CHECK confchk_session_create[] = {
	{ "allocation_size", "int",
	    NULL, "min=512B,max=128MB",
	    NULL, 0 },
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
	{ "block_allocation", "string",
	    NULL, "choices=[\"first\",\"best\"]",
	    NULL, 0 },
	{ "block_compressor", "string",
	    __wt_compressor_confchk, NULL,
	    NULL, 0 },
	{ "cache_resident", "boolean", NULL, NULL, NULL, 0 },
	{ "checksum", "string",
	    NULL, "choices=[\"on\",\"off\",\"uncompressed\"]",
	    NULL, 0 },
	{ "colgroups", "list", NULL, NULL, NULL, 0 },
	{ "collator", "string", __wt_collator_confchk, NULL, NULL, 0 },
	{ "columns", "list", NULL, NULL, NULL, 0 },
	{ "dictionary", "int", NULL, "min=0", NULL, 0 },
	{ "exclusive", "boolean", NULL, NULL, NULL, 0 },
	{ "extractor", "string",
	    __wt_extractor_confchk, NULL,
	    NULL, 0 },
	{ "format", "string", NULL, "choices=[\"btree\"]", NULL, 0 },
	{ "huffman_key", "string",
	    __wt_huffman_confchk, NULL,
	    NULL, 0 },
	{ "huffman_value", "string",
	    __wt_huffman_confchk, NULL,
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
	{ "lsm", "category", NULL, NULL, confchk_lsm_subconfigs, 11 },
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

static const WT_CONFIG_CHECK confchk_session_drop[] = {
	{ "force", "boolean", NULL, NULL, NULL, 0 },
	{ "remove_files", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_session_open_cursor[] = {
	{ "append", "boolean", NULL, NULL, NULL, 0 },
	{ "bulk", "string", NULL, NULL, NULL, 0 },
	{ "checkpoint", "string", NULL, NULL, NULL, 0 },
	{ "dump", "string",
	    NULL, "choices=[\"hex\",\"json\",\"print\"]",
	    NULL, 0 },
	{ "next_random", "boolean", NULL, NULL, NULL, 0 },
	{ "overwrite", "boolean", NULL, NULL, NULL, 0 },
	{ "raw", "boolean", NULL, NULL, NULL, 0 },
	{ "readonly", "boolean", NULL, NULL, NULL, 0 },
	{ "skip_sort_check", "boolean", NULL, NULL, NULL, 0 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"fast\",\"clear\"]",
	    NULL, 0 },
	{ "target", "list", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_session_reconfigure[] = {
	{ "isolation", "string",
	    NULL, "choices=[\"read-uncommitted\",\"read-committed\","
	    "\"snapshot\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_session_salvage[] = {
	{ "force", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_session_verify[] = {
	{ "dump_address", "boolean", NULL, NULL, NULL, 0 },
	{ "dump_blocks", "boolean", NULL, NULL, NULL, 0 },
	{ "dump_offsets", "list", NULL, NULL, NULL, 0 },
	{ "dump_pages", "boolean", NULL, NULL, NULL, 0 },
	{ "dump_shape", "boolean", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_table_meta[] = {
	{ "app_metadata", "string", NULL, NULL, NULL, 0 },
	{ "colgroups", "list", NULL, NULL, NULL, 0 },
	{ "collator", "string", __wt_collator_confchk, NULL, NULL, 0 },
	{ "columns", "list", NULL, NULL, NULL, 0 },
	{ "key_format", "format", __wt_struct_confchk, NULL, NULL, 0 },
	{ "value_format", "format",
	    __wt_struct_confchk, NULL,
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_log_subconfigs[] = {
	{ "archive", "boolean", NULL, NULL, NULL, 0 },
	{ "compressor", "string", NULL, NULL, NULL, 0 },
	{ "enabled", "boolean", NULL, NULL, NULL, 0 },
	{ "file_max", "int", NULL, "min=100KB,max=2GB", NULL, 0 },
	{ "path", "string", NULL, NULL, NULL, 0 },
	{ "prealloc", "boolean", NULL, NULL, NULL, 0 },
	{ "recover", "string",
	    NULL, "choices=[\"error\",\"on\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_transaction_sync_subconfigs[] = {
	{ "enabled", "boolean", NULL, NULL, NULL, 0 },
	{ "method", "string",
	    NULL, "choices=[\"dsync\",\"fsync\",\"none\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_wiredtiger_open[] = {
	{ "async", "category",
	    NULL, NULL,
	    confchk_async_subconfigs, 3 },
	{ "buffer_alignment", "int", NULL, "min=-1,max=1MB", NULL, 0 },
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_checkpoint_subconfigs, 3 },
	{ "checkpoint_sync", "boolean", NULL, NULL, NULL, 0 },
	{ "config_base", "boolean", NULL, NULL, NULL, 0 },
	{ "create", "boolean", NULL, NULL, NULL, 0 },
	{ "direct_io", "list",
	    NULL, "choices=[\"checkpoint\",\"data\",\"log\"]",
	    NULL, 0 },
	{ "error_prefix", "string", NULL, NULL, NULL, 0 },
	{ "eviction", "category",
	    NULL, NULL,
	    confchk_eviction_subconfigs, 2 },
	{ "eviction_dirty_target", "int",
	    NULL, "min=10,max=99",
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
	    confchk_file_manager_subconfigs, 3 },
	{ "hazard_max", "int", NULL, "min=15", NULL, 0 },
	{ "log", "category", NULL, NULL, confchk_log_subconfigs, 7 },
	{ "lsm_manager", "category",
	    NULL, NULL,
	    confchk_lsm_manager_subconfigs, 2 },
	{ "lsm_merge", "boolean", NULL, NULL, NULL, 0 },
	{ "mmap", "boolean", NULL, NULL, NULL, 0 },
	{ "multiprocess", "boolean", NULL, NULL, NULL, 0 },
	{ "session_max", "int", NULL, "min=1", NULL, 0 },
	{ "session_scratch_max", "int", NULL, NULL, NULL, 0 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_shared_cache_subconfigs, 4 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_statistics_log_subconfigs, 5 },
	{ "transaction_sync", "category",
	    NULL, NULL,
	    confchk_transaction_sync_subconfigs, 2 },
	{ "use_environment_priv", "boolean", NULL, NULL, NULL, 0 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\",\"compact\","
	    "\"evict\",\"evictserver\",\"fileops\",\"log\",\"lsm\","
	    "\"metadata\",\"mutex\",\"overflow\",\"read\",\"reconcile\","
	    "\"recovery\",\"salvage\",\"shared_cache\",\"split\","
	    "\"temporary\",\"transaction\",\"verify\",\"version\",\"write\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_all[] = {
	{ "async", "category",
	    NULL, NULL,
	    confchk_async_subconfigs, 3 },
	{ "buffer_alignment", "int", NULL, "min=-1,max=1MB", NULL, 0 },
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_checkpoint_subconfigs, 3 },
	{ "checkpoint_sync", "boolean", NULL, NULL, NULL, 0 },
	{ "config_base", "boolean", NULL, NULL, NULL, 0 },
	{ "create", "boolean", NULL, NULL, NULL, 0 },
	{ "direct_io", "list",
	    NULL, "choices=[\"checkpoint\",\"data\",\"log\"]",
	    NULL, 0 },
	{ "error_prefix", "string", NULL, NULL, NULL, 0 },
	{ "eviction", "category",
	    NULL, NULL,
	    confchk_eviction_subconfigs, 2 },
	{ "eviction_dirty_target", "int",
	    NULL, "min=10,max=99",
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
	    confchk_file_manager_subconfigs, 3 },
	{ "hazard_max", "int", NULL, "min=15", NULL, 0 },
	{ "log", "category", NULL, NULL, confchk_log_subconfigs, 7 },
	{ "lsm_manager", "category",
	    NULL, NULL,
	    confchk_lsm_manager_subconfigs, 2 },
	{ "lsm_merge", "boolean", NULL, NULL, NULL, 0 },
	{ "mmap", "boolean", NULL, NULL, NULL, 0 },
	{ "multiprocess", "boolean", NULL, NULL, NULL, 0 },
	{ "session_max", "int", NULL, "min=1", NULL, 0 },
	{ "session_scratch_max", "int", NULL, NULL, NULL, 0 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_shared_cache_subconfigs, 4 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_statistics_log_subconfigs, 5 },
	{ "transaction_sync", "category",
	    NULL, NULL,
	    confchk_transaction_sync_subconfigs, 2 },
	{ "use_environment_priv", "boolean", NULL, NULL, NULL, 0 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\",\"compact\","
	    "\"evict\",\"evictserver\",\"fileops\",\"log\",\"lsm\","
	    "\"metadata\",\"mutex\",\"overflow\",\"read\",\"reconcile\","
	    "\"recovery\",\"salvage\",\"shared_cache\",\"split\","
	    "\"temporary\",\"transaction\",\"verify\",\"version\",\"write\"]",
	    NULL, 0 },
	{ "version", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_basecfg[] = {
	{ "async", "category",
	    NULL, NULL,
	    confchk_async_subconfigs, 3 },
	{ "buffer_alignment", "int", NULL, "min=-1,max=1MB", NULL, 0 },
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_checkpoint_subconfigs, 3 },
	{ "checkpoint_sync", "boolean", NULL, NULL, NULL, 0 },
	{ "direct_io", "list",
	    NULL, "choices=[\"checkpoint\",\"data\",\"log\"]",
	    NULL, 0 },
	{ "error_prefix", "string", NULL, NULL, NULL, 0 },
	{ "eviction", "category",
	    NULL, NULL,
	    confchk_eviction_subconfigs, 2 },
	{ "eviction_dirty_target", "int",
	    NULL, "min=10,max=99",
	    NULL, 0 },
	{ "eviction_target", "int", NULL, "min=10,max=99", NULL, 0 },
	{ "eviction_trigger", "int", NULL, "min=10,max=99", NULL, 0 },
	{ "extensions", "list", NULL, NULL, NULL, 0 },
	{ "file_extend", "list",
	    NULL, "choices=[\"data\",\"log\"]",
	    NULL, 0 },
	{ "file_manager", "category",
	    NULL, NULL,
	    confchk_file_manager_subconfigs, 3 },
	{ "hazard_max", "int", NULL, "min=15", NULL, 0 },
	{ "log", "category", NULL, NULL, confchk_log_subconfigs, 7 },
	{ "lsm_manager", "category",
	    NULL, NULL,
	    confchk_lsm_manager_subconfigs, 2 },
	{ "lsm_merge", "boolean", NULL, NULL, NULL, 0 },
	{ "mmap", "boolean", NULL, NULL, NULL, 0 },
	{ "multiprocess", "boolean", NULL, NULL, NULL, 0 },
	{ "session_max", "int", NULL, "min=1", NULL, 0 },
	{ "session_scratch_max", "int", NULL, NULL, NULL, 0 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_shared_cache_subconfigs, 4 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_statistics_log_subconfigs, 5 },
	{ "transaction_sync", "category",
	    NULL, NULL,
	    confchk_transaction_sync_subconfigs, 2 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\",\"compact\","
	    "\"evict\",\"evictserver\",\"fileops\",\"log\",\"lsm\","
	    "\"metadata\",\"mutex\",\"overflow\",\"read\",\"reconcile\","
	    "\"recovery\",\"salvage\",\"shared_cache\",\"split\","
	    "\"temporary\",\"transaction\",\"verify\",\"version\",\"write\"]",
	    NULL, 0 },
	{ "version", "string", NULL, NULL, NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_usercfg[] = {
	{ "async", "category",
	    NULL, NULL,
	    confchk_async_subconfigs, 3 },
	{ "buffer_alignment", "int", NULL, "min=-1,max=1MB", NULL, 0 },
	{ "cache_overhead", "int", NULL, "min=0,max=30", NULL, 0 },
	{ "cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0 },
	{ "checkpoint", "category",
	    NULL, NULL,
	    confchk_checkpoint_subconfigs, 3 },
	{ "checkpoint_sync", "boolean", NULL, NULL, NULL, 0 },
	{ "direct_io", "list",
	    NULL, "choices=[\"checkpoint\",\"data\",\"log\"]",
	    NULL, 0 },
	{ "error_prefix", "string", NULL, NULL, NULL, 0 },
	{ "eviction", "category",
	    NULL, NULL,
	    confchk_eviction_subconfigs, 2 },
	{ "eviction_dirty_target", "int",
	    NULL, "min=10,max=99",
	    NULL, 0 },
	{ "eviction_target", "int", NULL, "min=10,max=99", NULL, 0 },
	{ "eviction_trigger", "int", NULL, "min=10,max=99", NULL, 0 },
	{ "extensions", "list", NULL, NULL, NULL, 0 },
	{ "file_extend", "list",
	    NULL, "choices=[\"data\",\"log\"]",
	    NULL, 0 },
	{ "file_manager", "category",
	    NULL, NULL,
	    confchk_file_manager_subconfigs, 3 },
	{ "hazard_max", "int", NULL, "min=15", NULL, 0 },
	{ "log", "category", NULL, NULL, confchk_log_subconfigs, 7 },
	{ "lsm_manager", "category",
	    NULL, NULL,
	    confchk_lsm_manager_subconfigs, 2 },
	{ "lsm_merge", "boolean", NULL, NULL, NULL, 0 },
	{ "mmap", "boolean", NULL, NULL, NULL, 0 },
	{ "multiprocess", "boolean", NULL, NULL, NULL, 0 },
	{ "session_max", "int", NULL, "min=1", NULL, 0 },
	{ "session_scratch_max", "int", NULL, NULL, NULL, 0 },
	{ "shared_cache", "category",
	    NULL, NULL,
	    confchk_shared_cache_subconfigs, 4 },
	{ "statistics", "list",
	    NULL, "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL, 0 },
	{ "statistics_log", "category",
	    NULL, NULL,
	    confchk_statistics_log_subconfigs, 5 },
	{ "transaction_sync", "category",
	    NULL, NULL,
	    confchk_transaction_sync_subconfigs, 2 },
	{ "verbose", "list",
	    NULL, "choices=[\"api\",\"block\",\"checkpoint\",\"compact\","
	    "\"evict\",\"evictserver\",\"fileops\",\"log\",\"lsm\","
	    "\"metadata\",\"mutex\",\"overflow\",\"read\",\"reconcile\","
	    "\"recovery\",\"salvage\",\"shared_cache\",\"split\","
	    "\"temporary\",\"transaction\",\"verify\",\"version\",\"write\"]",
	    NULL, 0 },
	{ NULL, NULL, NULL, NULL, NULL, 0 }
};

static const WT_CONFIG_ENTRY config_entries[] = {
	{ "colgroup.meta",
	  "app_metadata=,collator=,columns=,source=,type=file",
	  confchk_colgroup_meta, 5
	},
	{ "connection.add_collator",
	  "",
	  NULL, 0
	},
	{ "connection.add_compressor",
	  "",
	  NULL, 0
	},
	{ "connection.add_data_source",
	  "",
	  NULL, 0
	},
	{ "connection.add_extractor",
	  "",
	  NULL, 0
	},
	{ "connection.async_new_op",
	  "append=0,overwrite=,raw=0,timeout=1200",
	  confchk_connection_async_new_op, 4
	},
	{ "connection.close",
	  "leak_memory=0",
	  confchk_connection_close, 1
	},
	{ "connection.load_extension",
	  "config=,entry=wiredtiger_extension_init,"
	  "terminate=wiredtiger_extension_terminate",
	  confchk_connection_load_extension, 3
	},
	{ "connection.open_session",
	  "isolation=read-committed",
	  confchk_connection_open_session, 1
	},
	{ "connection.reconfigure",
	  "async=(enabled=0,ops_max=1024,threads=2),cache_overhead=8,"
	  "cache_size=100MB,checkpoint=(log_size=0,"
	  "name=\"WiredTigerCheckpoint\",wait=0),error_prefix=,"
	  "eviction=(threads_max=1,threads_min=1),eviction_dirty_target=80,"
	  "eviction_target=80,eviction_trigger=95,"
	  "file_manager=(close_idle_time=30,close_scan_interval=10,"
	  "open_handles=250),lsm_manager=(merge=,worker_thread_max=4),"
	  "lsm_merge=,shared_cache=(chunk=10MB,name=,reserve=0,size=500MB),"
	  "statistics=none,statistics_log=(on_close=0,"
	  "path=\"WiredTigerStat.%d.%H\",sources=,"
	  "timestamp=\"%b %d %H:%M:%S\",wait=0),verbose=",
	  confchk_connection_reconfigure, 16
	},
	{ "cursor.close",
	  "",
	  NULL, 0
	},
	{ "cursor.reconfigure",
	  "append=0,overwrite=",
	  confchk_cursor_reconfigure, 2
	},
	{ "file.meta",
	  "allocation_size=4KB,app_metadata=,block_allocation=best,"
	  "block_compressor=,cache_resident=0,checkpoint=,checkpoint_lsn=,"
	  "checksum=uncompressed,collator=,columns=,dictionary=0,"
	  "format=btree,huffman_key=,huffman_value=,id=,internal_item_max=0"
	  ",internal_key_max=0,internal_key_truncate=,internal_page_max=4KB"
	  ",key_format=u,key_gap=10,leaf_item_max=0,leaf_key_max=0,"
	  "leaf_page_max=32KB,leaf_value_max=0,memory_page_max=5MB,"
	  "os_cache_dirty_max=0,os_cache_max=0,prefix_compression=0,"
	  "prefix_compression_min=4,split_deepen_min_child=0,"
	  "split_deepen_per_child=0,split_pct=75,value_format=u,"
	  "version=(major=0,minor=0)",
	  confchk_file_meta, 35
	},
	{ "index.meta",
	  "app_metadata=,collator=,columns=,extractor=,immutable=0,"
	  "index_key_columns=,key_format=u,source=,type=file,value_format=u",
	  confchk_index_meta, 10
	},
	{ "session.begin_transaction",
	  "isolation=,name=,priority=0,sync=",
	  confchk_session_begin_transaction, 4
	},
	{ "session.checkpoint",
	  "drop=,force=0,name=,target=",
	  confchk_session_checkpoint, 4
	},
	{ "session.close",
	  "",
	  NULL, 0
	},
	{ "session.commit_transaction",
	  "",
	  NULL, 0
	},
	{ "session.compact",
	  "timeout=1200",
	  confchk_session_compact, 1
	},
	{ "session.create",
	  "allocation_size=4KB,app_metadata=,block_allocation=best,"
	  "block_compressor=,cache_resident=0,checksum=uncompressed,"
	  "colgroups=,collator=,columns=,dictionary=0,exclusive=0,"
	  "extractor=,format=btree,huffman_key=,huffman_value=,immutable=0,"
	  "internal_item_max=0,internal_key_max=0,internal_key_truncate=,"
	  "internal_page_max=4KB,key_format=u,key_gap=10,leaf_item_max=0,"
	  "leaf_key_max=0,leaf_page_max=32KB,leaf_value_max=0,"
	  "lsm=(auto_throttle=,bloom=,bloom_bit_count=16,bloom_config=,"
	  "bloom_hash_count=8,bloom_oldest=0,chunk_count_limit=0,"
	  "chunk_max=5GB,chunk_size=10MB,merge_max=15,merge_min=0),"
	  "memory_page_max=5MB,os_cache_dirty_max=0,os_cache_max=0,"
	  "prefix_compression=0,prefix_compression_min=4,source=,"
	  "split_deepen_min_child=0,split_deepen_per_child=0,split_pct=75,"
	  "type=file,value_format=u",
	  confchk_session_create, 38
	},
	{ "session.drop",
	  "force=0,remove_files=",
	  confchk_session_drop, 2
	},
	{ "session.log_printf",
	  "",
	  NULL, 0
	},
	{ "session.open_cursor",
	  "append=0,bulk=0,checkpoint=,dump=,next_random=0,overwrite=,raw=0"
	  ",readonly=0,skip_sort_check=0,statistics=,target=",
	  confchk_session_open_cursor, 11
	},
	{ "session.reconfigure",
	  "isolation=read-committed",
	  confchk_session_reconfigure, 1
	},
	{ "session.rename",
	  "",
	  NULL, 0
	},
	{ "session.rollback_transaction",
	  "",
	  NULL, 0
	},
	{ "session.salvage",
	  "force=0",
	  confchk_session_salvage, 1
	},
	{ "session.strerror",
	  "",
	  NULL, 0
	},
	{ "session.truncate",
	  "",
	  NULL, 0
	},
	{ "session.upgrade",
	  "",
	  NULL, 0
	},
	{ "session.verify",
	  "dump_address=0,dump_blocks=0,dump_offsets=,dump_pages=0,"
	  "dump_shape=0",
	  confchk_session_verify, 5
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
	  "config_base=,create=0,direct_io=,error_prefix=,"
	  "eviction=(threads_max=1,threads_min=1),eviction_dirty_target=80,"
	  "eviction_target=80,eviction_trigger=95,exclusive=0,extensions=,"
	  "file_extend=,file_manager=(close_idle_time=30,"
	  "close_scan_interval=10,open_handles=250),hazard_max=1000,"
	  "log=(archive=,compressor=,enabled=0,file_max=100MB,path=,"
	  "prealloc=,recover=on),lsm_manager=(merge=,worker_thread_max=4),"
	  "lsm_merge=,mmap=,multiprocess=0,session_max=100,"
	  "session_scratch_max=2MB,shared_cache=(chunk=10MB,name=,reserve=0"
	  ",size=500MB),statistics=none,statistics_log=(on_close=0,"
	  "path=\"WiredTigerStat.%d.%H\",sources=,"
	  "timestamp=\"%b %d %H:%M:%S\",wait=0),transaction_sync=(enabled=0"
	  ",method=fsync),use_environment_priv=0,verbose=",
	  confchk_wiredtiger_open, 32
	},
	{ "wiredtiger_open_all",
	  "async=(enabled=0,ops_max=1024,threads=2),buffer_alignment=-1,"
	  "cache_overhead=8,cache_size=100MB,checkpoint=(log_size=0,"
	  "name=\"WiredTigerCheckpoint\",wait=0),checkpoint_sync=,"
	  "config_base=,create=0,direct_io=,error_prefix=,"
	  "eviction=(threads_max=1,threads_min=1),eviction_dirty_target=80,"
	  "eviction_target=80,eviction_trigger=95,exclusive=0,extensions=,"
	  "file_extend=,file_manager=(close_idle_time=30,"
	  "close_scan_interval=10,open_handles=250),hazard_max=1000,"
	  "log=(archive=,compressor=,enabled=0,file_max=100MB,path=,"
	  "prealloc=,recover=on),lsm_manager=(merge=,worker_thread_max=4),"
	  "lsm_merge=,mmap=,multiprocess=0,session_max=100,"
	  "session_scratch_max=2MB,shared_cache=(chunk=10MB,name=,reserve=0"
	  ",size=500MB),statistics=none,statistics_log=(on_close=0,"
	  "path=\"WiredTigerStat.%d.%H\",sources=,"
	  "timestamp=\"%b %d %H:%M:%S\",wait=0),transaction_sync=(enabled=0"
	  ",method=fsync),use_environment_priv=0,verbose=,version=(major=0,"
	  "minor=0)",
	  confchk_wiredtiger_open_all, 33
	},
	{ "wiredtiger_open_basecfg",
	  "async=(enabled=0,ops_max=1024,threads=2),buffer_alignment=-1,"
	  "cache_overhead=8,cache_size=100MB,checkpoint=(log_size=0,"
	  "name=\"WiredTigerCheckpoint\",wait=0),checkpoint_sync=,"
	  "direct_io=,error_prefix=,eviction=(threads_max=1,threads_min=1),"
	  "eviction_dirty_target=80,eviction_target=80,eviction_trigger=95,"
	  "extensions=,file_extend=,file_manager=(close_idle_time=30,"
	  "close_scan_interval=10,open_handles=250),hazard_max=1000,"
	  "log=(archive=,compressor=,enabled=0,file_max=100MB,path=,"
	  "prealloc=,recover=on),lsm_manager=(merge=,worker_thread_max=4),"
	  "lsm_merge=,mmap=,multiprocess=0,session_max=100,"
	  "session_scratch_max=2MB,shared_cache=(chunk=10MB,name=,reserve=0"
	  ",size=500MB),statistics=none,statistics_log=(on_close=0,"
	  "path=\"WiredTigerStat.%d.%H\",sources=,"
	  "timestamp=\"%b %d %H:%M:%S\",wait=0),transaction_sync=(enabled=0"
	  ",method=fsync),verbose=,version=(major=0,minor=0)",
	  confchk_wiredtiger_open_basecfg, 29
	},
	{ "wiredtiger_open_usercfg",
	  "async=(enabled=0,ops_max=1024,threads=2),buffer_alignment=-1,"
	  "cache_overhead=8,cache_size=100MB,checkpoint=(log_size=0,"
	  "name=\"WiredTigerCheckpoint\",wait=0),checkpoint_sync=,"
	  "direct_io=,error_prefix=,eviction=(threads_max=1,threads_min=1),"
	  "eviction_dirty_target=80,eviction_target=80,eviction_trigger=95,"
	  "extensions=,file_extend=,file_manager=(close_idle_time=30,"
	  "close_scan_interval=10,open_handles=250),hazard_max=1000,"
	  "log=(archive=,compressor=,enabled=0,file_max=100MB,path=,"
	  "prealloc=,recover=on),lsm_manager=(merge=,worker_thread_max=4),"
	  "lsm_merge=,mmap=,multiprocess=0,session_max=100,"
	  "session_scratch_max=2MB,shared_cache=(chunk=10MB,name=,reserve=0"
	  ",size=500MB),statistics=none,statistics_log=(on_close=0,"
	  "path=\"WiredTigerStat.%d.%H\",sources=,"
	  "timestamp=\"%b %d %H:%M:%S\",wait=0),transaction_sync=(enabled=0"
	  ",method=fsync),verbose=",
	  confchk_wiredtiger_open_usercfg, 28
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
	WT_RET(__wt_calloc_def(session,
	    sizeof(config_entries) / sizeof(config_entries[0]), &epp));
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
