/* DO NOT EDIT: automatically built by dist/api_config.py. */

#include "wt_internal.h"

static const WT_CONFIG_CHECK confchk_colgroup_meta[] = {
	{ "app_metadata", "string", NULL, NULL },
	{ "columns", "list", NULL, NULL },
	{ "source", "string", NULL, NULL },
	{ "type", "string", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_connection_async_new_op[] = {
	{ "append", "boolean", NULL, NULL },
	{ "overwrite", "boolean", NULL, NULL },
	{ "raw", "boolean", NULL, NULL },
	{ "timeout", "int", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_connection_close[] = {
	{ "leak_memory", "boolean", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_connection_load_extension[] = {
	{ "config", "string", NULL, NULL },
	{ "entry", "string", NULL, NULL },
	{ "terminate", "string", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_connection_open_session[] = {
	{ "isolation", "string",
	    "choices=[\"read-uncommitted\",\"read-committed\",\"snapshot\"]",
	    NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_async_subconfigs[] = {
	{ "enabled", "boolean", NULL, NULL },
	{ "ops_max", "int", "min=10,max=4096", NULL },
	{ "threads", "int", "min=1,max=20", NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_checkpoint_subconfigs[] = {
	{ "log_size", "int", "min=0,max=2GB", NULL },
	{ "name", "string", NULL, NULL },
	{ "wait", "int", "min=0,max=100000", NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_eviction_subconfigs[] = {
	{ "threads_max", "int", "min=1,max=20", NULL },
	{ "threads_min", "int", "min=1,max=20", NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_lsm_manager_subconfigs[] = {
	{ "merge", "boolean", NULL, NULL },
	{ "worker_thread_max", "int", "min=3,max=20", NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_shared_cache_subconfigs[] = {
	{ "chunk", "int", "min=1MB,max=10TB", NULL },
	{ "name", "string", NULL, NULL },
	{ "reserve", "int", NULL, NULL },
	{ "size", "int", "min=1MB,max=10TB", NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_statistics_log_subconfigs[] = {
	{ "on_close", "boolean", NULL, NULL },
	{ "path", "string", NULL, NULL },
	{ "sources", "list", NULL, NULL },
	{ "timestamp", "string", NULL, NULL },
	{ "wait", "int", "min=0,max=100000", NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_connection_reconfigure[] = {
	{ "async", "category", NULL, confchk_async_subconfigs },
	{ "cache_size", "int", "min=1MB,max=10TB", NULL },
	{ "checkpoint", "category", NULL,
	     confchk_checkpoint_subconfigs },
	{ "error_prefix", "string", NULL, NULL },
	{ "eviction", "category", NULL, confchk_eviction_subconfigs },
	{ "eviction_dirty_target", "int", "min=10,max=99", NULL },
	{ "eviction_target", "int", "min=10,max=99", NULL },
	{ "eviction_trigger", "int", "min=10,max=99", NULL },
	{ "lsm_manager", "category", NULL,
	     confchk_lsm_manager_subconfigs },
	{ "lsm_merge", "boolean", NULL, NULL },
	{ "shared_cache", "category", NULL,
	     confchk_shared_cache_subconfigs },
	{ "statistics", "list",
	    "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL },
	{ "statistics_log", "category", NULL,
	     confchk_statistics_log_subconfigs },
	{ "verbose", "list",
	    "choices=[\"api\",\"block\",\"checkpoint\",\"compact\",\"evict\""
	    ",\"evictserver\",\"fileops\",\"log\",\"lsm\",\"metadata\","
	    "\"mutex\",\"overflow\",\"read\",\"reconcile\",\"recovery\","
	    "\"salvage\",\"shared_cache\",\"split\",\"temporary\","
	    "\"transaction\",\"verify\",\"version\",\"write\"]",
	    NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_file_meta[] = {
	{ "allocation_size", "int", "min=512B,max=128MB", NULL },
	{ "app_metadata", "string", NULL, NULL },
	{ "block_allocation", "string",
	    "choices=[\"first\",\"best\"]",
	    NULL },
	{ "block_compressor", "string", NULL, NULL },
	{ "cache_resident", "boolean", NULL, NULL },
	{ "checkpoint", "string", NULL, NULL },
	{ "checkpoint_lsn", "string", NULL, NULL },
	{ "checksum", "string",
	    "choices=[\"on\",\"off\",\"uncompressed\"]",
	    NULL },
	{ "collator", "string", NULL, NULL },
	{ "columns", "list", NULL, NULL },
	{ "dictionary", "int", "min=0", NULL },
	{ "format", "string", "choices=[\"btree\"]", NULL },
	{ "huffman_key", "string", NULL, NULL },
	{ "huffman_value", "string", NULL, NULL },
	{ "id", "string", NULL, NULL },
	{ "internal_item_max", "int", "min=0", NULL },
	{ "internal_key_max", "int", "min=0", NULL },
	{ "internal_key_truncate", "boolean", NULL, NULL },
	{ "internal_page_max", "int", "min=512B,max=512MB", NULL },
	{ "key_format", "format", NULL, NULL },
	{ "key_gap", "int", "min=0", NULL },
	{ "leaf_item_max", "int", "min=0", NULL },
	{ "leaf_key_max", "int", "min=0", NULL },
	{ "leaf_page_max", "int", "min=512B,max=512MB", NULL },
	{ "leaf_value_max", "int", "min=0", NULL },
	{ "memory_page_max", "int", "min=512B,max=10TB", NULL },
	{ "os_cache_dirty_max", "int", "min=0", NULL },
	{ "os_cache_max", "int", "min=0", NULL },
	{ "prefix_compression", "boolean", NULL, NULL },
	{ "prefix_compression_min", "int", "min=0", NULL },
	{ "split_pct", "int", "min=25,max=100", NULL },
	{ "value_format", "format", NULL, NULL },
	{ "version", "string", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_index_meta[] = {
	{ "app_metadata", "string", NULL, NULL },
	{ "columns", "list", NULL, NULL },
	{ "extractor", "string", NULL, NULL },
	{ "immutable", "boolean", NULL, NULL },
	{ "index_key_columns", "int", NULL, NULL },
	{ "key_format", "format", NULL, NULL },
	{ "source", "string", NULL, NULL },
	{ "type", "string", NULL, NULL },
	{ "value_format", "format", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_session_begin_transaction[] = {
	{ "isolation", "string",
	    "choices=[\"read-uncommitted\",\"read-committed\",\"snapshot\"]",
	    NULL },
	{ "name", "string", NULL, NULL },
	{ "priority", "int", "min=-100,max=100", NULL },
	{ "sync", "boolean", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_session_checkpoint[] = {
	{ "drop", "list", NULL, NULL },
	{ "force", "boolean", NULL, NULL },
	{ "name", "string", NULL, NULL },
	{ "target", "list", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_session_compact[] = {
	{ "timeout", "int", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_lsm_subconfigs[] = {
	{ "auto_throttle", "boolean", NULL, NULL },
	{ "bloom", "boolean", NULL, NULL },
	{ "bloom_bit_count", "int", "min=2,max=1000", NULL },
	{ "bloom_config", "string", NULL, NULL },
	{ "bloom_hash_count", "int", "min=2,max=100", NULL },
	{ "bloom_oldest", "boolean", NULL, NULL },
	{ "chunk_max", "int", "min=100MB,max=10TB", NULL },
	{ "chunk_size", "int", "min=512K,max=500MB", NULL },
	{ "merge_max", "int", "min=2,max=100", NULL },
	{ "merge_min", "int", "max=100", NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_session_create[] = {
	{ "allocation_size", "int", "min=512B,max=128MB", NULL },
	{ "app_metadata", "string", NULL, NULL },
	{ "block_allocation", "string",
	    "choices=[\"first\",\"best\"]",
	    NULL },
	{ "block_compressor", "string", NULL, NULL },
	{ "cache_resident", "boolean", NULL, NULL },
	{ "checksum", "string",
	    "choices=[\"on\",\"off\",\"uncompressed\"]",
	    NULL },
	{ "colgroups", "list", NULL, NULL },
	{ "collator", "string", NULL, NULL },
	{ "columns", "list", NULL, NULL },
	{ "dictionary", "int", "min=0", NULL },
	{ "exclusive", "boolean", NULL, NULL },
	{ "extractor", "string", NULL, NULL },
	{ "format", "string", "choices=[\"btree\"]", NULL },
	{ "huffman_key", "string", NULL, NULL },
	{ "huffman_value", "string", NULL, NULL },
	{ "immutable", "boolean", NULL, NULL },
	{ "internal_item_max", "int", "min=0", NULL },
	{ "internal_key_max", "int", "min=0", NULL },
	{ "internal_key_truncate", "boolean", NULL, NULL },
	{ "internal_page_max", "int", "min=512B,max=512MB", NULL },
	{ "key_format", "format", NULL, NULL },
	{ "key_gap", "int", "min=0", NULL },
	{ "leaf_item_max", "int", "min=0", NULL },
	{ "leaf_key_max", "int", "min=0", NULL },
	{ "leaf_page_max", "int", "min=512B,max=512MB", NULL },
	{ "leaf_value_max", "int", "min=0", NULL },
	{ "lsm", "category", NULL, confchk_lsm_subconfigs },
	{ "memory_page_max", "int", "min=512B,max=10TB", NULL },
	{ "os_cache_dirty_max", "int", "min=0", NULL },
	{ "os_cache_max", "int", "min=0", NULL },
	{ "prefix_compression", "boolean", NULL, NULL },
	{ "prefix_compression_min", "int", "min=0", NULL },
	{ "source", "string", NULL, NULL },
	{ "split_pct", "int", "min=25,max=100", NULL },
	{ "type", "string", NULL, NULL },
	{ "value_format", "format", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_session_drop[] = {
	{ "force", "boolean", NULL, NULL },
	{ "remove_files", "boolean", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_session_open_cursor[] = {
	{ "append", "boolean", NULL, NULL },
	{ "bulk", "string", NULL, NULL },
	{ "checkpoint", "string", NULL, NULL },
	{ "dump", "string",
	    "choices=[\"hex\",\"json\",\"print\"]",
	    NULL },
	{ "next_random", "boolean", NULL, NULL },
	{ "overwrite", "boolean", NULL, NULL },
	{ "raw", "boolean", NULL, NULL },
	{ "readonly", "boolean", NULL, NULL },
	{ "skip_sort_check", "boolean", NULL, NULL },
	{ "statistics", "list",
	    "choices=[\"all\",\"fast\",\"clear\"]",
	    NULL },
	{ "target", "list", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_session_reconfigure[] = {
	{ "isolation", "string",
	    "choices=[\"read-uncommitted\",\"read-committed\",\"snapshot\"]",
	    NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_session_salvage[] = {
	{ "force", "boolean", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_session_verify[] = {
	{ "dump_address", "boolean", NULL, NULL },
	{ "dump_blocks", "boolean", NULL, NULL },
	{ "dump_offsets", "list", NULL, NULL },
	{ "dump_pages", "boolean", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_table_meta[] = {
	{ "app_metadata", "string", NULL, NULL },
	{ "colgroups", "list", NULL, NULL },
	{ "columns", "list", NULL, NULL },
	{ "key_format", "format", NULL, NULL },
	{ "value_format", "format", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_log_subconfigs[] = {
	{ "archive", "boolean", NULL, NULL },
	{ "compressor", "string", NULL, NULL },
	{ "enabled", "boolean", NULL, NULL },
	{ "file_max", "int", "min=100KB,max=2GB", NULL },
	{ "path", "string", NULL, NULL },
	{ "prealloc", "boolean", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_transaction_sync_subconfigs[] = {
	{ "enabled", "boolean", NULL, NULL },
	{ "method", "string",
	    "choices=[\"dsync\",\"fsync\",\"none\"]",
	    NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_wiredtiger_open[] = {
	{ "async", "category", NULL, confchk_async_subconfigs },
	{ "buffer_alignment", "int", "min=-1,max=1MB", NULL },
	{ "cache_size", "int", "min=1MB,max=10TB", NULL },
	{ "checkpoint", "category", NULL,
	     confchk_checkpoint_subconfigs },
	{ "checkpoint_sync", "boolean", NULL, NULL },
	{ "config_base", "boolean", NULL, NULL },
	{ "create", "boolean", NULL, NULL },
	{ "direct_io", "list",
	    "choices=[\"checkpoint\",\"data\",\"log\"]",
	    NULL },
	{ "error_prefix", "string", NULL, NULL },
	{ "eviction", "category", NULL, confchk_eviction_subconfigs },
	{ "eviction_dirty_target", "int", "min=10,max=99", NULL },
	{ "eviction_target", "int", "min=10,max=99", NULL },
	{ "eviction_trigger", "int", "min=10,max=99", NULL },
	{ "exclusive", "boolean", NULL, NULL },
	{ "extensions", "list", NULL, NULL },
	{ "file_extend", "list", "choices=[\"data\",\"log\"]", NULL },
	{ "hazard_max", "int", "min=15", NULL },
	{ "log", "category", NULL, confchk_log_subconfigs },
	{ "lsm_manager", "category", NULL,
	     confchk_lsm_manager_subconfigs },
	{ "lsm_merge", "boolean", NULL, NULL },
	{ "mmap", "boolean", NULL, NULL },
	{ "multiprocess", "boolean", NULL, NULL },
	{ "session_max", "int", "min=1", NULL },
	{ "shared_cache", "category", NULL,
	     confchk_shared_cache_subconfigs },
	{ "statistics", "list",
	    "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL },
	{ "statistics_log", "category", NULL,
	     confchk_statistics_log_subconfigs },
	{ "transaction_sync", "category", NULL,
	     confchk_transaction_sync_subconfigs },
	{ "use_environment_priv", "boolean", NULL, NULL },
	{ "verbose", "list",
	    "choices=[\"api\",\"block\",\"checkpoint\",\"compact\",\"evict\""
	    ",\"evictserver\",\"fileops\",\"log\",\"lsm\",\"metadata\","
	    "\"mutex\",\"overflow\",\"read\",\"reconcile\",\"recovery\","
	    "\"salvage\",\"shared_cache\",\"split\",\"temporary\","
	    "\"transaction\",\"verify\",\"version\",\"write\"]",
	    NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_all[] = {
	{ "async", "category", NULL, confchk_async_subconfigs },
	{ "buffer_alignment", "int", "min=-1,max=1MB", NULL },
	{ "cache_size", "int", "min=1MB,max=10TB", NULL },
	{ "checkpoint", "category", NULL,
	     confchk_checkpoint_subconfigs },
	{ "checkpoint_sync", "boolean", NULL, NULL },
	{ "config_base", "boolean", NULL, NULL },
	{ "create", "boolean", NULL, NULL },
	{ "direct_io", "list",
	    "choices=[\"checkpoint\",\"data\",\"log\"]",
	    NULL },
	{ "error_prefix", "string", NULL, NULL },
	{ "eviction", "category", NULL, confchk_eviction_subconfigs },
	{ "eviction_dirty_target", "int", "min=10,max=99", NULL },
	{ "eviction_target", "int", "min=10,max=99", NULL },
	{ "eviction_trigger", "int", "min=10,max=99", NULL },
	{ "exclusive", "boolean", NULL, NULL },
	{ "extensions", "list", NULL, NULL },
	{ "file_extend", "list", "choices=[\"data\",\"log\"]", NULL },
	{ "hazard_max", "int", "min=15", NULL },
	{ "log", "category", NULL, confchk_log_subconfigs },
	{ "lsm_manager", "category", NULL,
	     confchk_lsm_manager_subconfigs },
	{ "lsm_merge", "boolean", NULL, NULL },
	{ "mmap", "boolean", NULL, NULL },
	{ "multiprocess", "boolean", NULL, NULL },
	{ "session_max", "int", "min=1", NULL },
	{ "shared_cache", "category", NULL,
	     confchk_shared_cache_subconfigs },
	{ "statistics", "list",
	    "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL },
	{ "statistics_log", "category", NULL,
	     confchk_statistics_log_subconfigs },
	{ "transaction_sync", "category", NULL,
	     confchk_transaction_sync_subconfigs },
	{ "use_environment_priv", "boolean", NULL, NULL },
	{ "verbose", "list",
	    "choices=[\"api\",\"block\",\"checkpoint\",\"compact\",\"evict\""
	    ",\"evictserver\",\"fileops\",\"log\",\"lsm\",\"metadata\","
	    "\"mutex\",\"overflow\",\"read\",\"reconcile\",\"recovery\","
	    "\"salvage\",\"shared_cache\",\"split\",\"temporary\","
	    "\"transaction\",\"verify\",\"version\",\"write\"]",
	    NULL },
	{ "version", "string", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_basecfg[] = {
	{ "async", "category", NULL, confchk_async_subconfigs },
	{ "buffer_alignment", "int", "min=-1,max=1MB", NULL },
	{ "cache_size", "int", "min=1MB,max=10TB", NULL },
	{ "checkpoint", "category", NULL,
	     confchk_checkpoint_subconfigs },
	{ "checkpoint_sync", "boolean", NULL, NULL },
	{ "direct_io", "list",
	    "choices=[\"checkpoint\",\"data\",\"log\"]",
	    NULL },
	{ "error_prefix", "string", NULL, NULL },
	{ "eviction", "category", NULL, confchk_eviction_subconfigs },
	{ "eviction_dirty_target", "int", "min=10,max=99", NULL },
	{ "eviction_target", "int", "min=10,max=99", NULL },
	{ "eviction_trigger", "int", "min=10,max=99", NULL },
	{ "extensions", "list", NULL, NULL },
	{ "file_extend", "list", "choices=[\"data\",\"log\"]", NULL },
	{ "hazard_max", "int", "min=15", NULL },
	{ "log", "category", NULL, confchk_log_subconfigs },
	{ "lsm_manager", "category", NULL,
	     confchk_lsm_manager_subconfigs },
	{ "lsm_merge", "boolean", NULL, NULL },
	{ "mmap", "boolean", NULL, NULL },
	{ "multiprocess", "boolean", NULL, NULL },
	{ "session_max", "int", "min=1", NULL },
	{ "shared_cache", "category", NULL,
	     confchk_shared_cache_subconfigs },
	{ "statistics", "list",
	    "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL },
	{ "statistics_log", "category", NULL,
	     confchk_statistics_log_subconfigs },
	{ "transaction_sync", "category", NULL,
	     confchk_transaction_sync_subconfigs },
	{ "verbose", "list",
	    "choices=[\"api\",\"block\",\"checkpoint\",\"compact\",\"evict\""
	    ",\"evictserver\",\"fileops\",\"log\",\"lsm\",\"metadata\","
	    "\"mutex\",\"overflow\",\"read\",\"reconcile\",\"recovery\","
	    "\"salvage\",\"shared_cache\",\"split\",\"temporary\","
	    "\"transaction\",\"verify\",\"version\",\"write\"]",
	    NULL },
	{ "version", "string", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_usercfg[] = {
	{ "async", "category", NULL, confchk_async_subconfigs },
	{ "buffer_alignment", "int", "min=-1,max=1MB", NULL },
	{ "cache_size", "int", "min=1MB,max=10TB", NULL },
	{ "checkpoint", "category", NULL,
	     confchk_checkpoint_subconfigs },
	{ "checkpoint_sync", "boolean", NULL, NULL },
	{ "direct_io", "list",
	    "choices=[\"checkpoint\",\"data\",\"log\"]",
	    NULL },
	{ "error_prefix", "string", NULL, NULL },
	{ "eviction", "category", NULL, confchk_eviction_subconfigs },
	{ "eviction_dirty_target", "int", "min=10,max=99", NULL },
	{ "eviction_target", "int", "min=10,max=99", NULL },
	{ "eviction_trigger", "int", "min=10,max=99", NULL },
	{ "extensions", "list", NULL, NULL },
	{ "file_extend", "list", "choices=[\"data\",\"log\"]", NULL },
	{ "hazard_max", "int", "min=15", NULL },
	{ "log", "category", NULL, confchk_log_subconfigs },
	{ "lsm_manager", "category", NULL,
	     confchk_lsm_manager_subconfigs },
	{ "lsm_merge", "boolean", NULL, NULL },
	{ "mmap", "boolean", NULL, NULL },
	{ "multiprocess", "boolean", NULL, NULL },
	{ "session_max", "int", "min=1", NULL },
	{ "shared_cache", "category", NULL,
	     confchk_shared_cache_subconfigs },
	{ "statistics", "list",
	    "choices=[\"all\",\"fast\",\"none\",\"clear\"]",
	    NULL },
	{ "statistics_log", "category", NULL,
	     confchk_statistics_log_subconfigs },
	{ "transaction_sync", "category", NULL,
	     confchk_transaction_sync_subconfigs },
	{ "verbose", "list",
	    "choices=[\"api\",\"block\",\"checkpoint\",\"compact\",\"evict\""
	    ",\"evictserver\",\"fileops\",\"log\",\"lsm\",\"metadata\","
	    "\"mutex\",\"overflow\",\"read\",\"reconcile\",\"recovery\","
	    "\"salvage\",\"shared_cache\",\"split\",\"temporary\","
	    "\"transaction\",\"verify\",\"version\",\"write\"]",
	    NULL },
	{ NULL, NULL, NULL, NULL }
};

static const WT_CONFIG_ENTRY config_entries[] = {
	{ "colgroup.meta",
	  "app_metadata=,columns=,source=,type=file",
	  confchk_colgroup_meta
	},
	{ "connection.add_collator",
	  "",
	  NULL
	},
	{ "connection.add_compressor",
	  "",
	  NULL
	},
	{ "connection.add_data_source",
	  "",
	  NULL
	},
	{ "connection.add_extractor",
	  "",
	  NULL
	},
	{ "connection.async_new_op",
	  "append=0,overwrite=,raw=0,timeout=1200",
	  confchk_connection_async_new_op
	},
	{ "connection.close",
	  "leak_memory=0",
	  confchk_connection_close
	},
	{ "connection.load_extension",
	  "config=,entry=wiredtiger_extension_init,"
	  "terminate=wiredtiger_extension_terminate",
	  confchk_connection_load_extension
	},
	{ "connection.open_session",
	  "isolation=read-committed",
	  confchk_connection_open_session
	},
	{ "connection.reconfigure",
	  "async=(enabled=0,ops_max=1024,threads=2),cache_size=100MB,"
	  "checkpoint=(log_size=0,name=\"WiredTigerCheckpoint\",wait=0),"
	  "error_prefix=,eviction=(threads_max=1,threads_min=1),"
	  "eviction_dirty_target=80,eviction_target=80,eviction_trigger=95,"
	  "lsm_manager=(merge=,worker_thread_max=4),lsm_merge=,"
	  "shared_cache=(chunk=10MB,name=none,reserve=0,size=500MB),"
	  "statistics=none,statistics_log=(on_close=0,"
	  "path=\"WiredTigerStat.%d.%H\",sources=,"
	  "timestamp=\"%b %d %H:%M:%S\",wait=0),verbose=",
	  confchk_connection_reconfigure
	},
	{ "cursor.close",
	  "",
	  NULL
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
	  "prefix_compression_min=4,split_pct=75,value_format=u,"
	  "version=(major=0,minor=0)",
	  confchk_file_meta
	},
	{ "index.meta",
	  "app_metadata=,columns=,extractor=,immutable=0,index_key_columns="
	  ",key_format=u,source=,type=file,value_format=u",
	  confchk_index_meta
	},
	{ "session.begin_transaction",
	  "isolation=,name=,priority=0,sync=",
	  confchk_session_begin_transaction
	},
	{ "session.checkpoint",
	  "drop=,force=0,name=,target=",
	  confchk_session_checkpoint
	},
	{ "session.close",
	  "",
	  NULL
	},
	{ "session.commit_transaction",
	  "",
	  NULL
	},
	{ "session.compact",
	  "timeout=1200",
	  confchk_session_compact
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
	  "bloom_hash_count=8,bloom_oldest=0,chunk_max=5GB,chunk_size=10MB,"
	  "merge_max=15,merge_min=0),memory_page_max=5MB,"
	  "os_cache_dirty_max=0,os_cache_max=0,prefix_compression=0,"
	  "prefix_compression_min=4,source=,split_pct=75,type=file,"
	  "value_format=u",
	  confchk_session_create
	},
	{ "session.drop",
	  "force=0,remove_files=",
	  confchk_session_drop
	},
	{ "session.log_printf",
	  "",
	  NULL
	},
	{ "session.open_cursor",
	  "append=0,bulk=0,checkpoint=,dump=,next_random=0,overwrite=,raw=0"
	  ",readonly=0,skip_sort_check=0,statistics=,target=",
	  confchk_session_open_cursor
	},
	{ "session.reconfigure",
	  "isolation=read-committed",
	  confchk_session_reconfigure
	},
	{ "session.rename",
	  "",
	  NULL
	},
	{ "session.rollback_transaction",
	  "",
	  NULL
	},
	{ "session.salvage",
	  "force=0",
	  confchk_session_salvage
	},
	{ "session.truncate",
	  "",
	  NULL
	},
	{ "session.upgrade",
	  "",
	  NULL
	},
	{ "session.verify",
	  "dump_address=0,dump_blocks=0,dump_offsets=,dump_pages=0",
	  confchk_session_verify
	},
	{ "table.meta",
	  "app_metadata=,colgroups=,columns=,key_format=u,value_format=u",
	  confchk_table_meta
	},
	{ "wiredtiger_open",
	  "async=(enabled=0,ops_max=1024,threads=2),buffer_alignment=-1,"
	  "cache_size=100MB,checkpoint=(log_size=0,"
	  "name=\"WiredTigerCheckpoint\",wait=0),checkpoint_sync=,"
	  "config_base=,create=0,direct_io=,error_prefix=,"
	  "eviction=(threads_max=1,threads_min=1),eviction_dirty_target=80,"
	  "eviction_target=80,eviction_trigger=95,exclusive=0,extensions=,"
	  "file_extend=,hazard_max=1000,log=(archive=,compressor=,enabled=0"
	  ",file_max=100MB,path=,prealloc=),lsm_manager=(merge=,"
	  "worker_thread_max=4),lsm_merge=,mmap=,multiprocess=0,"
	  "session_max=100,shared_cache=(chunk=10MB,name=none,reserve=0,"
	  "size=500MB),statistics=none,statistics_log=(on_close=0,"
	  "path=\"WiredTigerStat.%d.%H\",sources=,"
	  "timestamp=\"%b %d %H:%M:%S\",wait=0),transaction_sync=(enabled=0"
	  ",method=fsync),use_environment_priv=0,verbose=",
	  confchk_wiredtiger_open
	},
	{ "wiredtiger_open_all",
	  "async=(enabled=0,ops_max=1024,threads=2),buffer_alignment=-1,"
	  "cache_size=100MB,checkpoint=(log_size=0,"
	  "name=\"WiredTigerCheckpoint\",wait=0),checkpoint_sync=,"
	  "config_base=,create=0,direct_io=,error_prefix=,"
	  "eviction=(threads_max=1,threads_min=1),eviction_dirty_target=80,"
	  "eviction_target=80,eviction_trigger=95,exclusive=0,extensions=,"
	  "file_extend=,hazard_max=1000,log=(archive=,compressor=,enabled=0"
	  ",file_max=100MB,path=,prealloc=),lsm_manager=(merge=,"
	  "worker_thread_max=4),lsm_merge=,mmap=,multiprocess=0,"
	  "session_max=100,shared_cache=(chunk=10MB,name=none,reserve=0,"
	  "size=500MB),statistics=none,statistics_log=(on_close=0,"
	  "path=\"WiredTigerStat.%d.%H\",sources=,"
	  "timestamp=\"%b %d %H:%M:%S\",wait=0),transaction_sync=(enabled=0"
	  ",method=fsync),use_environment_priv=0,verbose=,version=(major=0,"
	  "minor=0)",
	  confchk_wiredtiger_open_all
	},
	{ "wiredtiger_open_basecfg",
	  "async=(enabled=0,ops_max=1024,threads=2),buffer_alignment=-1,"
	  "cache_size=100MB,checkpoint=(log_size=0,"
	  "name=\"WiredTigerCheckpoint\",wait=0),checkpoint_sync=,"
	  "direct_io=,error_prefix=,eviction=(threads_max=1,threads_min=1),"
	  "eviction_dirty_target=80,eviction_target=80,eviction_trigger=95,"
	  "extensions=,file_extend=,hazard_max=1000,log=(archive=,"
	  "compressor=,enabled=0,file_max=100MB,path=,prealloc=),"
	  "lsm_manager=(merge=,worker_thread_max=4),lsm_merge=,mmap=,"
	  "multiprocess=0,session_max=100,shared_cache=(chunk=10MB,"
	  "name=none,reserve=0,size=500MB),statistics=none,"
	  "statistics_log=(on_close=0,path=\"WiredTigerStat.%d.%H\","
	  "sources=,timestamp=\"%b %d %H:%M:%S\",wait=0),"
	  "transaction_sync=(enabled=0,method=fsync),verbose=,"
	  "version=(major=0,minor=0)",
	  confchk_wiredtiger_open_basecfg
	},
	{ "wiredtiger_open_usercfg",
	  "async=(enabled=0,ops_max=1024,threads=2),buffer_alignment=-1,"
	  "cache_size=100MB,checkpoint=(log_size=0,"
	  "name=\"WiredTigerCheckpoint\",wait=0),checkpoint_sync=,"
	  "direct_io=,error_prefix=,eviction=(threads_max=1,threads_min=1),"
	  "eviction_dirty_target=80,eviction_target=80,eviction_trigger=95,"
	  "extensions=,file_extend=,hazard_max=1000,log=(archive=,"
	  "compressor=,enabled=0,file_max=100MB,path=,prealloc=),"
	  "lsm_manager=(merge=,worker_thread_max=4),lsm_merge=,mmap=,"
	  "multiprocess=0,session_max=100,shared_cache=(chunk=10MB,"
	  "name=none,reserve=0,size=500MB),statistics=none,"
	  "statistics_log=(on_close=0,path=\"WiredTigerStat.%d.%H\","
	  "sources=,timestamp=\"%b %d %H:%M:%S\",wait=0),"
	  "transaction_sync=(enabled=0,method=fsync),verbose=",
	  confchk_wiredtiger_open_usercfg
	},
	{ NULL, NULL, NULL }
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
