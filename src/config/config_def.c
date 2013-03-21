/* DO NOT EDIT: automatically built by dist/config.py. */

#include "wt_internal.h"

const char *
__wt_confdfl_colgroup_meta =
	"columns=,source=,type=file";

WT_CONFIG_CHECK
__wt_confchk_colgroup_meta[] = {
	{ "columns", "list", NULL, NULL},
	{ "source", "string", NULL, NULL},
	{ "type", "string", "choices=[\"file\",\"lsm\"]", NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_add_collator =
	"";

WT_CONFIG_CHECK
__wt_confchk_connection_add_collator[] = {
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_add_compressor =
	"";

WT_CONFIG_CHECK
__wt_confchk_connection_add_compressor[] = {
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_add_data_source =
	"";

WT_CONFIG_CHECK
__wt_confchk_connection_add_data_source[] = {
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_add_extractor =
	"";

WT_CONFIG_CHECK
__wt_confchk_connection_add_extractor[] = {
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_close =
	"";

WT_CONFIG_CHECK
__wt_confchk_connection_close[] = {
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_load_extension =
	"entry=wiredtiger_extension_init,prefix=";

WT_CONFIG_CHECK
__wt_confchk_connection_load_extension[] = {
	{ "entry", "string", NULL, NULL},
	{ "prefix", "string", NULL, NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_open_session =
	"isolation=read-committed";

WT_CONFIG_CHECK
__wt_confchk_connection_open_session[] = {
	{ "isolation", "string",
	    "choices=[\"read-uncommitted\",\"read-committed\",\"snapshot\"]",
	    NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_reconfigure =
	"cache_size=100MB,error_prefix=,eviction_dirty_target=80,"
	"eviction_target=80,eviction_trigger=95,shared_cache=(chunk=10MB,"
	"name=pool,reserve=0,size=500MB),statistics=0,verbose=";

WT_CONFIG_CHECK
__wt_confchk_shared_cache_subconfigs[] = {
	{ "chunk", "int", "min=1MB,max=10TB", NULL },
	{ "name", "string", NULL, NULL },
	{ "reserve", "int", NULL, NULL },
	{ "size", "int", "min=1MB,max=10TB", NULL },
	{ NULL, NULL, NULL, NULL }
};

WT_CONFIG_CHECK
__wt_confchk_connection_reconfigure[] = {
	{ "cache_size", "int", "min=1MB,max=10TB", NULL},
	{ "error_prefix", "string", NULL, NULL},
	{ "eviction_dirty_target", "int", "min=10,max=99", NULL},
	{ "eviction_target", "int", "min=10,max=99", NULL},
	{ "eviction_trigger", "int", "min=10,max=99", NULL},
	{ "shared_cache", "category", NULL,
	     __wt_confchk_shared_cache_subconfigs},
	{ "statistics", "boolean", NULL, NULL},
	{ "verbose", "list",
	    "choices=[\"block\",\"shared_cache\",\"ckpt\",\"evict\","
	    "\"evictserver\",\"fileops\",\"hazard\",\"lsm\",\"mutex\",\"read\","
	    "\"readserver\",\"reconcile\",\"salvage\",\"verify\",\"write\"]",
	    NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_cursor_close =
	"";

WT_CONFIG_CHECK
__wt_confchk_cursor_close[] = {
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_file_meta =
	"allocation_size=512B,block_compressor=,cache_resident=0,checkpoint=,"
	"checksum=on,collator=,columns=,dictionary=0,format=btree,"
	"huffman_key=,huffman_value=,internal_item_max=0,"
	"internal_key_truncate=,internal_page_max=2KB,key_format=u,key_gap=10"
	",leaf_item_max=0,leaf_page_max=1MB,memory_page_max=5MB,"
	"os_cache_dirty_max=0,os_cache_max=0,prefix_compression=,split_pct=75"
	",value_format=u,version=(major=0,minor=0)";

WT_CONFIG_CHECK
__wt_confchk_file_meta[] = {
	{ "allocation_size", "int", "min=512B,max=128MB", NULL},
	{ "block_compressor", "string", NULL, NULL},
	{ "cache_resident", "boolean", NULL, NULL},
	{ "checkpoint", "string", NULL, NULL},
	{ "checksum", "string",
	    "choices=[\"on\",\"off\",\"uncompressed\"]",
	    NULL},
	{ "collator", "string", NULL, NULL},
	{ "columns", "list", NULL, NULL},
	{ "dictionary", "int", "min=0", NULL},
	{ "format", "string", "choices=[\"btree\"]", NULL},
	{ "huffman_key", "string", NULL, NULL},
	{ "huffman_value", "string", NULL, NULL},
	{ "internal_item_max", "int", "min=0", NULL},
	{ "internal_key_truncate", "boolean", NULL, NULL},
	{ "internal_page_max", "int", "min=512B,max=512MB", NULL},
	{ "key_format", "format", NULL, NULL},
	{ "key_gap", "int", "min=0", NULL},
	{ "leaf_item_max", "int", "min=0", NULL},
	{ "leaf_page_max", "int", "min=512B,max=512MB", NULL},
	{ "memory_page_max", "int", "min=512B,max=10TB", NULL},
	{ "os_cache_dirty_max", "int", "min=0", NULL},
	{ "os_cache_max", "int", "min=0", NULL},
	{ "prefix_compression", "boolean", NULL, NULL},
	{ "split_pct", "int", "min=25,max=100", NULL},
	{ "value_format", "format", NULL, NULL},
	{ "version", "string", NULL, NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_index_meta =
	"columns=,key_format=u,source=,type=file,value_format=u";

WT_CONFIG_CHECK
__wt_confchk_index_meta[] = {
	{ "columns", "list", NULL, NULL},
	{ "key_format", "format", NULL, NULL},
	{ "source", "string", NULL, NULL},
	{ "type", "string", "choices=[\"file\",\"lsm\"]", NULL},
	{ "value_format", "format", NULL, NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_begin_transaction =
	"isolation=,name=,priority=0,sync=full";

WT_CONFIG_CHECK
__wt_confchk_session_begin_transaction[] = {
	{ "isolation", "string",
	    "choices=[\"read-uncommitted\",\"read-committed\",\"snapshot\"]",
	    NULL},
	{ "name", "string", NULL, NULL},
	{ "priority", "int", "min=-100,max=100", NULL},
	{ "sync", "string",
	    "choices=[\"full\",\"flush\",\"write\",\"none\"]",
	    NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_checkpoint =
	"drop=,force=0,name=,target=";

WT_CONFIG_CHECK
__wt_confchk_session_checkpoint[] = {
	{ "drop", "list", NULL, NULL},
	{ "force", "boolean", NULL, NULL},
	{ "name", "string", NULL, NULL},
	{ "target", "list", NULL, NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_close =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_close[] = {
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_commit_transaction =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_commit_transaction[] = {
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_compact =
	"trigger=30";

WT_CONFIG_CHECK
__wt_confchk_session_compact[] = {
	{ "trigger", "int", "min=10,max=50", NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_create =
	"allocation_size=512B,block_compressor=,cache_resident=0,checksum=on,"
	"colgroups=,collator=,columns=,dictionary=0,exclusive=0,format=btree,"
	"huffman_key=,huffman_value=,internal_item_max=0,"
	"internal_key_truncate=,internal_page_max=2KB,key_format=u,key_gap=10"
	",leaf_item_max=0,leaf_page_max=1MB,lsm_bloom=,lsm_bloom_bit_count=8,"
	"lsm_bloom_config=,lsm_bloom_hash_count=4,lsm_bloom_newest=0,"
	"lsm_bloom_oldest=0,lsm_chunk_size=2MB,lsm_merge_max=15,"
	"lsm_merge_threads=1,memory_page_max=5MB,os_cache_dirty_max=0,"
	"os_cache_max=0,prefix_compression=,source=,split_pct=75,type=file,"
	"value_format=u";

WT_CONFIG_CHECK
__wt_confchk_session_create[] = {
	{ "allocation_size", "int", "min=512B,max=128MB", NULL},
	{ "block_compressor", "string", NULL, NULL},
	{ "cache_resident", "boolean", NULL, NULL},
	{ "checksum", "string",
	    "choices=[\"on\",\"off\",\"uncompressed\"]",
	    NULL},
	{ "colgroups", "list", NULL, NULL},
	{ "collator", "string", NULL, NULL},
	{ "columns", "list", NULL, NULL},
	{ "dictionary", "int", "min=0", NULL},
	{ "exclusive", "boolean", NULL, NULL},
	{ "format", "string", "choices=[\"btree\"]", NULL},
	{ "huffman_key", "string", NULL, NULL},
	{ "huffman_value", "string", NULL, NULL},
	{ "internal_item_max", "int", "min=0", NULL},
	{ "internal_key_truncate", "boolean", NULL, NULL},
	{ "internal_page_max", "int", "min=512B,max=512MB", NULL},
	{ "key_format", "format", NULL, NULL},
	{ "key_gap", "int", "min=0", NULL},
	{ "leaf_item_max", "int", "min=0", NULL},
	{ "leaf_page_max", "int", "min=512B,max=512MB", NULL},
	{ "lsm_bloom", "boolean", NULL, NULL},
	{ "lsm_bloom_bit_count", "int", "min=2,max=1000", NULL},
	{ "lsm_bloom_config", "string", NULL, NULL},
	{ "lsm_bloom_hash_count", "int", "min=2,max=100", NULL},
	{ "lsm_bloom_newest", "boolean", NULL, NULL},
	{ "lsm_bloom_oldest", "boolean", NULL, NULL},
	{ "lsm_chunk_size", "int", "min=512K,max=500MB", NULL},
	{ "lsm_merge_max", "int", "min=2,max=100", NULL},
	{ "lsm_merge_threads", "int", "min=1,max=10", NULL},
	{ "memory_page_max", "int", "min=512B,max=10TB", NULL},
	{ "os_cache_dirty_max", "int", "min=0", NULL},
	{ "os_cache_max", "int", "min=0", NULL},
	{ "prefix_compression", "boolean", NULL, NULL},
	{ "source", "string", NULL, NULL},
	{ "split_pct", "int", "min=25,max=100", NULL},
	{ "type", "string", "choices=[\"file\",\"lsm\"]", NULL},
	{ "value_format", "format", NULL, NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_drop =
	"force=0,remove_files=";

WT_CONFIG_CHECK
__wt_confchk_session_drop[] = {
	{ "force", "boolean", NULL, NULL},
	{ "remove_files", "boolean", NULL, NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_log_printf =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_log_printf[] = {
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_open_cursor =
	"append=0,bulk=0,checkpoint=,dump=,next_random=0,overwrite=0,raw=0,"
	"statistics_clear=0,statistics_fast=0,target=";

WT_CONFIG_CHECK
__wt_confchk_session_open_cursor[] = {
	{ "append", "boolean", NULL, NULL},
	{ "bulk", "string", NULL, NULL},
	{ "checkpoint", "string", NULL, NULL},
	{ "dump", "string", "choices=[\"hex\",\"print\"]", NULL},
	{ "next_random", "boolean", NULL, NULL},
	{ "overwrite", "boolean", NULL, NULL},
	{ "raw", "boolean", NULL, NULL},
	{ "statistics_clear", "boolean", NULL, NULL},
	{ "statistics_fast", "boolean", NULL, NULL},
	{ "target", "list", NULL, NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_reconfigure =
	"isolation=read-committed";

WT_CONFIG_CHECK
__wt_confchk_session_reconfigure[] = {
	{ "isolation", "string",
	    "choices=[\"read-uncommitted\",\"read-committed\",\"snapshot\"]",
	    NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_rename =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_rename[] = {
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_rollback_transaction =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_rollback_transaction[] = {
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_salvage =
	"force=0";

WT_CONFIG_CHECK
__wt_confchk_session_salvage[] = {
	{ "force", "boolean", NULL, NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_truncate =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_truncate[] = {
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_upgrade =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_upgrade[] = {
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_verify =
	"dump_address=0,dump_blocks=0,dump_pages=0";

WT_CONFIG_CHECK
__wt_confchk_session_verify[] = {
	{ "dump_address", "boolean", NULL, NULL},
	{ "dump_blocks", "boolean", NULL, NULL},
	{ "dump_pages", "boolean", NULL, NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_table_meta =
	"colgroups=,columns=,key_format=u,value_format=u";

WT_CONFIG_CHECK
__wt_confchk_table_meta[] = {
	{ "colgroups", "list", NULL, NULL},
	{ "columns", "list", NULL, NULL},
	{ "key_format", "format", NULL, NULL},
	{ "value_format", "format", NULL, NULL},
	{ NULL, NULL, NULL, NULL }
};

const char *
__wt_confdfl_wiredtiger_open =
	"buffer_alignment=-1,cache_size=100MB,"
	"checkpoint=(name=\"WiredTigerCheckpoint\",wait=0),create=0,"
	"direct_io=,error_prefix=,eviction_dirty_target=80,eviction_target=80"
	",eviction_trigger=95,extensions=,hazard_max=1000,logging=0,"
	"lsm_merge=,mmap=,multiprocess=0,session_max=50,"
	"shared_cache=(chunk=10MB,name=pool,reserve=0,size=500MB),"
	"statistics=0,statistics_log=(clear=,path=\"WiredTigerStat.%H\","
	"timestamp=\"%b %d %H:%M:%S\",wait=0),sync=,transactional=,"
	"use_environment_priv=0,verbose=";

WT_CONFIG_CHECK
__wt_confchk_checkpoint_subconfigs[] = {
	{ "name", "string", NULL, NULL },
	{ "wait", "int", "min=1,max=100000", NULL },
	{ NULL, NULL, NULL, NULL }
};

WT_CONFIG_CHECK
__wt_confchk_statistics_log_subconfigs[] = {
	{ "clear", "boolean", NULL, NULL },
	{ "path", "string", NULL, NULL },
	{ "timestamp", "string", NULL, NULL },
	{ "wait", "int", "min=5,max=100000", NULL },
	{ NULL, NULL, NULL, NULL }
};

WT_CONFIG_CHECK
__wt_confchk_wiredtiger_open[] = {
	{ "buffer_alignment", "int", "min=-1,max=1MB", NULL},
	{ "cache_size", "int", "min=1MB,max=10TB", NULL},
	{ "checkpoint", "category", NULL,
	     __wt_confchk_checkpoint_subconfigs},
	{ "create", "boolean", NULL, NULL},
	{ "direct_io", "list", "choices=[\"data\",\"log\"]", NULL},
	{ "error_prefix", "string", NULL, NULL},
	{ "eviction_dirty_target", "int", "min=10,max=99", NULL},
	{ "eviction_target", "int", "min=10,max=99", NULL},
	{ "eviction_trigger", "int", "min=10,max=99", NULL},
	{ "extensions", "list", NULL, NULL},
	{ "hazard_max", "int", "min=15", NULL},
	{ "logging", "boolean", NULL, NULL},
	{ "lsm_merge", "boolean", NULL, NULL},
	{ "mmap", "boolean", NULL, NULL},
	{ "multiprocess", "boolean", NULL, NULL},
	{ "session_max", "int", "min=1", NULL},
	{ "shared_cache", "category", NULL,
	     __wt_confchk_shared_cache_subconfigs},
	{ "statistics", "boolean", NULL, NULL},
	{ "statistics_log", "category", NULL,
	     __wt_confchk_statistics_log_subconfigs},
	{ "sync", "boolean", NULL, NULL},
	{ "transactional", "boolean", NULL, NULL},
	{ "use_environment_priv", "boolean", NULL, NULL},
	{ "verbose", "list",
	    "choices=[\"block\",\"shared_cache\",\"ckpt\",\"evict\","
	    "\"evictserver\",\"fileops\",\"hazard\",\"lsm\",\"mutex\",\"read\","
	    "\"readserver\",\"reconcile\",\"salvage\",\"verify\",\"write\"]",
	    NULL},
	{ NULL, NULL, NULL, NULL }
};
