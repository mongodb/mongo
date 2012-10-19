/* DO NOT EDIT: automatically built by dist/config.py. */

#include "wt_internal.h"

const char *
__wt_confdfl_colgroup_meta =
	"columns=,source=,type=file";

WT_CONFIG_CHECK
__wt_confchk_colgroup_meta[] = {
	{ "columns", "list", NULL },
	{ "source", "string", NULL },
	{ "type", "string", "choices=[\"file\",\"lsm\"]" },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_add_collator =
	"";

WT_CONFIG_CHECK
__wt_confchk_connection_add_collator[] = {
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_add_compressor =
	"";

WT_CONFIG_CHECK
__wt_confchk_connection_add_compressor[] = {
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_add_data_source =
	"";

WT_CONFIG_CHECK
__wt_confchk_connection_add_data_source[] = {
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_add_extractor =
	"";

WT_CONFIG_CHECK
__wt_confchk_connection_add_extractor[] = {
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_close =
	"";

WT_CONFIG_CHECK
__wt_confchk_connection_close[] = {
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_load_extension =
	"entry=wiredtiger_extension_init,prefix=";

WT_CONFIG_CHECK
__wt_confchk_connection_load_extension[] = {
	{ "entry", "string", NULL },
	{ "prefix", "string", NULL },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_open_session =
	"isolation=read-committed";

WT_CONFIG_CHECK
__wt_confchk_connection_open_session[] = {
	{ "isolation", "string", "choices=[\"read-uncommitted\","
	    "\"read-committed\",\"snapshot\"]" },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_connection_reconfigure =
	"cache_size=100MB,error_prefix=,eviction_target=80,"
	"eviction_trigger=95,verbose=";

WT_CONFIG_CHECK
__wt_confchk_connection_reconfigure[] = {
	{ "cache_size", "int", "min=1MB,max=10TB" },
	{ "error_prefix", "string", NULL },
	{ "eviction_target", "int", "min=10,max=99" },
	{ "eviction_trigger", "int", "min=10,max=99" },
	{ "verbose", "list", "choices=[\"block\",\"ckpt\",\"evict\","
	    "\"evictserver\",\"fileops\",\"hazard\",\"lsm\",\"mutex\",\"read\","
	    "\"readserver\",\"reconcile\",\"salvage\",\"verify\",\"write\"]" },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_cursor_close =
	"";

WT_CONFIG_CHECK
__wt_confchk_cursor_close[] = {
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_file_meta =
	"allocation_size=512B,block_compressor=,cache_resident=0,checkpoint=,"
	"checksum=,collator=,columns=,dictionary=0,format=btree,huffman_key=,"
	"huffman_value=,internal_item_max=0,internal_key_truncate=,"
	"internal_page_max=2KB,key_format=u,key_gap=10,leaf_item_max=0,"
	"leaf_page_max=1MB,lsm_bloom=,lsm_bloom_bit_count=8,"
	"lsm_bloom_hash_count=4,lsm_bloom_newest=0,lsm_bloom_oldest=0,"
	"lsm_chunk_size=2MB,lsm_merge_max=15,prefix_compression=,split_pct=75"
	",value_format=u,version=(major=0,minor=0)";

WT_CONFIG_CHECK
__wt_confchk_file_meta[] = {
	{ "allocation_size", "int", "min=512B,max=128MB" },
	{ "block_compressor", "string", NULL },
	{ "cache_resident", "boolean", NULL },
	{ "checkpoint", "string", NULL },
	{ "checksum", "boolean", NULL },
	{ "collator", "string", NULL },
	{ "columns", "list", NULL },
	{ "dictionary", "int", "min=0" },
	{ "format", "string", "choices=[\"btree\"]" },
	{ "huffman_key", "string", NULL },
	{ "huffman_value", "string", NULL },
	{ "internal_item_max", "int", "min=0" },
	{ "internal_key_truncate", "boolean", NULL },
	{ "internal_page_max", "int", "min=512B,max=512MB" },
	{ "key_format", "format", NULL },
	{ "key_gap", "int", "min=0" },
	{ "leaf_item_max", "int", "min=0" },
	{ "leaf_page_max", "int", "min=512B,max=512MB" },
	{ "lsm_bloom", "boolean", NULL },
	{ "lsm_bloom_bit_count", "int", "min=2,max=1000" },
	{ "lsm_bloom_hash_count", "int", "min=2,max=100" },
	{ "lsm_bloom_newest", "boolean", NULL },
	{ "lsm_bloom_oldest", "boolean", NULL },
	{ "lsm_chunk_size", "int", "min=512K,max=500MB" },
	{ "lsm_merge_max", "int", "min=2,max=100" },
	{ "prefix_compression", "boolean", NULL },
	{ "split_pct", "int", "min=25,max=100" },
	{ "value_format", "format", NULL },
	{ "version", "string", NULL },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_index_meta =
	"columns=,columns=,key_format=u,source=,type=file,value_format=u";

WT_CONFIG_CHECK
__wt_confchk_index_meta[] = {
	{ "columns", "list", NULL },
	{ "columns", "list", NULL },
	{ "key_format", "format", NULL },
	{ "source", "string", NULL },
	{ "type", "string", "choices=[\"file\",\"lsm\"]" },
	{ "value_format", "format", NULL },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_begin_transaction =
	"isolation=,name=,priority=0,sync=full";

WT_CONFIG_CHECK
__wt_confchk_session_begin_transaction[] = {
	{ "isolation", "string", "choices=[\"read-uncommitted\","
	    "\"read-committed\",\"snapshot\"]" },
	{ "name", "string", NULL },
	{ "priority", "int", "min=-100,max=100" },
	{ "sync", "string", "choices=[\"full\",\"flush\",\"write\","
	    "\"none\"]" },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_checkpoint =
	"drop=,force=0,name=,target=";

WT_CONFIG_CHECK
__wt_confchk_session_checkpoint[] = {
	{ "drop", "list", NULL },
	{ "force", "boolean", NULL },
	{ "name", "string", NULL },
	{ "target", "list", NULL },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_close =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_close[] = {
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_commit_transaction =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_commit_transaction[] = {
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_compact =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_compact[] = {
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_create =
	"allocation_size=512B,block_compressor=,cache_resident=0,checksum=,"
	"colgroups=,collator=,columns=,columns=,dictionary=0,exclusive=0,"
	"format=btree,huffman_key=,huffman_value=,internal_item_max=0,"
	"internal_key_truncate=,internal_page_max=2KB,key_format=u,"
	"key_format=u,key_gap=10,leaf_item_max=0,leaf_page_max=1MB,lsm_bloom="
	",lsm_bloom_bit_count=8,lsm_bloom_hash_count=4,lsm_bloom_newest=0,"
	"lsm_bloom_oldest=0,lsm_chunk_size=2MB,lsm_merge_max=15,"
	"prefix_compression=,source=,split_pct=75,type=file,value_format=u,"
	"value_format=u";

WT_CONFIG_CHECK
__wt_confchk_session_create[] = {
	{ "allocation_size", "int", "min=512B,max=128MB" },
	{ "block_compressor", "string", NULL },
	{ "cache_resident", "boolean", NULL },
	{ "checksum", "boolean", NULL },
	{ "colgroups", "list", NULL },
	{ "collator", "string", NULL },
	{ "columns", "list", NULL },
	{ "columns", "list", NULL },
	{ "dictionary", "int", "min=0" },
	{ "exclusive", "boolean", NULL },
	{ "format", "string", "choices=[\"btree\"]" },
	{ "huffman_key", "string", NULL },
	{ "huffman_value", "string", NULL },
	{ "internal_item_max", "int", "min=0" },
	{ "internal_key_truncate", "boolean", NULL },
	{ "internal_page_max", "int", "min=512B,max=512MB" },
	{ "key_format", "format", NULL },
	{ "key_format", "format", NULL },
	{ "key_gap", "int", "min=0" },
	{ "leaf_item_max", "int", "min=0" },
	{ "leaf_page_max", "int", "min=512B,max=512MB" },
	{ "lsm_bloom", "boolean", NULL },
	{ "lsm_bloom_bit_count", "int", "min=2,max=1000" },
	{ "lsm_bloom_hash_count", "int", "min=2,max=100" },
	{ "lsm_bloom_newest", "boolean", NULL },
	{ "lsm_bloom_oldest", "boolean", NULL },
	{ "lsm_chunk_size", "int", "min=512K,max=500MB" },
	{ "lsm_merge_max", "int", "min=2,max=100" },
	{ "prefix_compression", "boolean", NULL },
	{ "source", "string", NULL },
	{ "split_pct", "int", "min=25,max=100" },
	{ "type", "string", "choices=[\"file\",\"lsm\"]" },
	{ "value_format", "format", NULL },
	{ "value_format", "format", NULL },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_drop =
	"force=0";

WT_CONFIG_CHECK
__wt_confchk_session_drop[] = {
	{ "force", "boolean", NULL },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_log_printf =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_log_printf[] = {
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_open_cursor =
	"append=0,bulk=0,checkpoint=,dump=,next_random=0,no_cache=0,"
	"overwrite=0,raw=0,statistics=0,statistics_clear=0,statistics_fast=0,"
	"target=";

WT_CONFIG_CHECK
__wt_confchk_session_open_cursor[] = {
	{ "append", "boolean", NULL },
	{ "bulk", "string", NULL },
	{ "checkpoint", "string", NULL },
	{ "dump", "string", "choices=[\"hex\",\"print\"]" },
	{ "next_random", "boolean", NULL },
	{ "no_cache", "boolean", NULL },
	{ "overwrite", "boolean", NULL },
	{ "raw", "boolean", NULL },
	{ "statistics", "boolean", NULL },
	{ "statistics_clear", "boolean", NULL },
	{ "statistics_fast", "boolean", NULL },
	{ "target", "list", NULL },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_reconfigure =
	"isolation=read-committed";

WT_CONFIG_CHECK
__wt_confchk_session_reconfigure[] = {
	{ "isolation", "string", "choices=[\"read-uncommitted\","
	    "\"read-committed\",\"snapshot\"]" },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_rename =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_rename[] = {
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_rollback_transaction =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_rollback_transaction[] = {
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_salvage =
	"force=0";

WT_CONFIG_CHECK
__wt_confchk_session_salvage[] = {
	{ "force", "boolean", NULL },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_truncate =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_truncate[] = {
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_upgrade =
	"";

WT_CONFIG_CHECK
__wt_confchk_session_upgrade[] = {
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_session_verify =
	"dump_address=0,dump_blocks=0,dump_pages=0";

WT_CONFIG_CHECK
__wt_confchk_session_verify[] = {
	{ "dump_address", "boolean", NULL },
	{ "dump_blocks", "boolean", NULL },
	{ "dump_pages", "boolean", NULL },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_table_meta =
	"colgroups=,columns=,key_format=u,value_format=u";

WT_CONFIG_CHECK
__wt_confchk_table_meta[] = {
	{ "colgroups", "list", NULL },
	{ "columns", "list", NULL },
	{ "key_format", "format", NULL },
	{ "value_format", "format", NULL },
	{ NULL, NULL, NULL }
};

const char *
__wt_confdfl_wiredtiger_open =
	"buffer_alignment=-1,cache_size=100MB,create=0,direct_io=,"
	"error_prefix=,eviction_target=80,eviction_trigger=95,extensions=,"
	"hazard_max=1000,logging=0,lsm_merge=,multiprocess=0,session_max=50,"
	"sync=,transactional=,use_environment_priv=0,verbose=";

WT_CONFIG_CHECK
__wt_confchk_wiredtiger_open[] = {
	{ "buffer_alignment", "int", "min=-1,max=1MB" },
	{ "cache_size", "int", "min=1MB,max=10TB" },
	{ "create", "boolean", NULL },
	{ "direct_io", "list", "choices=[\"data\",\"log\"]" },
	{ "error_prefix", "string", NULL },
	{ "eviction_target", "int", "min=10,max=99" },
	{ "eviction_trigger", "int", "min=10,max=99" },
	{ "extensions", "list", NULL },
	{ "hazard_max", "int", "min=15" },
	{ "logging", "boolean", NULL },
	{ "lsm_merge", "boolean", NULL },
	{ "multiprocess", "boolean", NULL },
	{ "session_max", "int", "min=1" },
	{ "sync", "boolean", NULL },
	{ "transactional", "boolean", NULL },
	{ "use_environment_priv", "boolean", NULL },
	{ "verbose", "list", "choices=[\"block\",\"ckpt\",\"evict\","
	    "\"evictserver\",\"fileops\",\"hazard\",\"lsm\",\"mutex\",\"read\","
	    "\"readserver\",\"reconcile\",\"salvage\",\"verify\",\"write\"]" },
	{ NULL, NULL, NULL }
};
