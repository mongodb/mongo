/* DO NOT EDIT: automatically built by dist/config.py. */

#include "wt_internal.h"

const char *
__wt_confdfl_colgroup_meta =
    "columns=,filename=";

const char *
__wt_confchk_colgroup_meta =
    "columns=(type=list),filename=()";

const char *
__wt_confdfl_connection_add_collator =
    "";

const char *
__wt_confchk_connection_add_collator =
    "";

const char *
__wt_confdfl_connection_add_compressor =
    "";

const char *
__wt_confchk_connection_add_compressor =
    "";

const char *
__wt_confdfl_connection_add_cursor_type =
    "";

const char *
__wt_confchk_connection_add_cursor_type =
    "";

const char *
__wt_confdfl_connection_add_extractor =
    "";

const char *
__wt_confchk_connection_add_extractor =
    "";

const char *
__wt_confdfl_connection_close =
    "";

const char *
__wt_confchk_connection_close =
    "";

const char *
__wt_confdfl_connection_load_extension =
    "entry=wiredtiger_extension_init,prefix=";

const char *
__wt_confchk_connection_load_extension =
    "entry=(),prefix=()";

const char *
__wt_confdfl_connection_open_session =
    "";

const char *
__wt_confchk_connection_open_session =
    "";

const char *
__wt_confdfl_cursor_close =
    "clear=false";

const char *
__wt_confchk_cursor_close =
    "clear=(type=boolean)";

const char *
__wt_confdfl_file_meta =
    "allocation_size=512B,block_compressor=,columns=,huffman_key=,"
    "huffman_value=,internal_key_truncate=true,internal_node_max=2KB,"
    "internal_node_min=2KB,key_format=u,key_gap=10,leaf_node_max=1MB,"
    "leaf_node_min=32KB,prefix_compression=true,split_min=false,split_pct=75,"
    "type=btree,value_format=u";

const char *
__wt_confchk_file_meta =
    "allocation_size=(type=int,min=512B,max=128MB),block_compressor=(),"
    "columns=(type=list),huffman_key=(),huffman_value=(),"
    "internal_key_truncate=(type=boolean),internal_node_max=(type=int,"
    "min=512B,max=512MB),internal_node_min=(type=int,min=512B,max=512MB),"
    "key_format=(type=format),key_gap=(type=int,min=0),"
    "leaf_node_max=(type=int,min=512B,max=512MB),leaf_node_min=(type=int,"
    "min=512B,max=512MB),prefix_compression=(type=boolean),"
    "split_min=(type=boolean),split_pct=(type=int,min=0,max=100),"
    "type=(choices=[\"btree\"]),value_format=(type=format)";

const char *
__wt_confdfl_index_meta =
    "columns=,filename=";

const char *
__wt_confchk_index_meta =
    "columns=(type=list),filename=()";

const char *
__wt_confdfl_session_begin_transaction =
    "isolation=read-committed,name=,priority=0,sync=full";

const char *
__wt_confchk_session_begin_transaction =
    "isolation=(choices=[\"serializable\",\"snapshot\",\"read-committed\","
    "\"read-uncommitted\"]),name=(),priority=(type=int,min=-100,max=100),"
    "sync=(choices=[\"full\",\"flush\",\"write\",\"none\"])";

const char *
__wt_confdfl_session_checkpoint =
    "archive=false,flush_cache=true,flush_log=true,force=false,log_size=0,"
    "timeout=0";

const char *
__wt_confchk_session_checkpoint =
    "archive=(type=boolean),flush_cache=(type=boolean),"
    "flush_log=(type=boolean),force=(type=boolean),log_size=(type=int,min=0),"
    "timeout=(type=int,min=0)";

const char *
__wt_confdfl_session_close =
    "";

const char *
__wt_confchk_session_close =
    "";

const char *
__wt_confdfl_session_commit_transaction =
    "";

const char *
__wt_confchk_session_commit_transaction =
    "";

const char *
__wt_confdfl_session_create =
    "allocation_size=512B,block_compressor=,colgroups=,columns=,columns=,"
    "exclusive=false,filename=,huffman_key=,huffman_value=,"
    "internal_key_truncate=true,internal_node_max=2KB,internal_node_min=2KB,"
    "key_format=u,key_format=u,key_gap=10,leaf_node_max=1MB,"
    "leaf_node_min=32KB,prefix_compression=true,split_min=false,split_pct=75,"
    "type=btree,value_format=u,value_format=u";

const char *
__wt_confchk_session_create =
    "allocation_size=(type=int,min=512B,max=128MB),block_compressor=(),"
    "colgroups=(),columns=(type=list),columns=(type=list),"
    "exclusive=(type=boolean),filename=(),huffman_key=(),huffman_value=(),"
    "internal_key_truncate=(type=boolean),internal_node_max=(type=int,"
    "min=512B,max=512MB),internal_node_min=(type=int,min=512B,max=512MB),"
    "key_format=(type=format),key_format=(type=format),key_gap=(type=int,"
    "min=0),leaf_node_max=(type=int,min=512B,max=512MB),"
    "leaf_node_min=(type=int,min=512B,max=512MB),"
    "prefix_compression=(type=boolean),split_min=(type=boolean),"
    "split_pct=(type=int,min=0,max=100),type=(choices=[\"btree\"]),"
    "value_format=(type=format),value_format=(type=format)";

const char *
__wt_confdfl_session_drop =
    "force=false";

const char *
__wt_confchk_session_drop =
    "force=(type=boolean)";

const char *
__wt_confdfl_session_dumpfile =
    "";

const char *
__wt_confchk_session_dumpfile =
    "";

const char *
__wt_confdfl_session_log_printf =
    "";

const char *
__wt_confchk_session_log_printf =
    "";

const char *
__wt_confdfl_session_open_cursor =
    "bulk=false,dump=false,isolation=read-committed,overwrite=false,"
    "printable=false,raw=false,statistics=false";

const char *
__wt_confchk_session_open_cursor =
    "bulk=(type=boolean),dump=(type=boolean),isolation=(choices=[\"snapshot\""
    ",\"read-committed\",\"read-uncommitted\"]),overwrite=(type=boolean),"
    "printable=(type=boolean),raw=(type=boolean),statistics=(type=boolean)";

const char *
__wt_confdfl_session_rename =
    "";

const char *
__wt_confchk_session_rename =
    "";

const char *
__wt_confdfl_session_rollback_transaction =
    "";

const char *
__wt_confchk_session_rollback_transaction =
    "";

const char *
__wt_confdfl_session_salvage =
    "force=false";

const char *
__wt_confchk_session_salvage =
    "force=(type=boolean)";

const char *
__wt_confdfl_session_sync =
    "";

const char *
__wt_confchk_session_sync =
    "";

const char *
__wt_confdfl_session_truncate =
    "";

const char *
__wt_confchk_session_truncate =
    "";

const char *
__wt_confdfl_session_verify =
    "";

const char *
__wt_confchk_session_verify =
    "";

const char *
__wt_confdfl_table_meta =
    "colgroups=,columns=,key_format=u,value_format=u";

const char *
__wt_confchk_table_meta =
    "colgroups=(),columns=(type=list),key_format=(type=format),"
    "value_format=(type=format)";

const char *
__wt_confdfl_wiredtiger_open =
    "cache_size=20MB,create=false,error_prefix=,exclusive=false,extensions=,"
    "hazard_max=30,logging=false,multiprocess=false,session_max=50,verbose=";

const char *
__wt_confchk_wiredtiger_open =
    "cache_size=(type=int,min=1MB,max=10TB),create=(type=boolean),"
    "error_prefix=(),exclusive=(type=boolean),extensions=(type=list),"
    "hazard_max=(type=int,min=15),logging=(type=boolean),"
    "multiprocess=(type=boolean),session_max=(type=int,min=1),"
    "verbose=(type=list,choices=[\"allocate\",\"evictserver\",\"fileops\","
    "\"hazard\",\"mutex\",\"read\",\"readserver\",\"reconcile\",\"salvage\","
    "\"write\"])";
