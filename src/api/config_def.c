/* DO NOT EDIT: automatically built by dist/config.py. */

#include "wt_internal.h"

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
    "";

const char *
__wt_confchk_cursor_close =
    "";

const char *
__wt_confdfl_session_begin_transaction =
    "isolation=read-committed,name=,sync=full,priority=0";

const char *
__wt_confchk_session_begin_transaction =
    "isolation=(choices=[\"serializable\",\"snapshot\",\"read-committed\","
    "\"read-uncommitted\"]),name=(),sync=(choices=[\"full\",\"flush\","
    "\"write\",\"none\"]),priority=(type=int,min=-100,max=100)";

const char *
__wt_confdfl_session_checkpoint =
    "archive=false,force=false,flush_cache=true,flush_log=true,log_size=0,"
    "timeout=0";

const char *
__wt_confchk_session_checkpoint =
    "archive=(type=boolean),force=(type=boolean),flush_cache=(type=boolean),"
    "flush_log=(type=boolean),log_size=(type=int,min=0),timeout=(type=int,"
    "min=0)";

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
    "allocation_size=512B,btree_huffman_key=,btree_huffman_value=,"
    "btree_key_gap=10,btree_internal_key_truncate=true,"
    "btree_prefix_compression=true,btree_split_min=false,btree_split_pct=75,"
    "colgroup.name=,columns=,exclusive=false,index.name=,intl_node_max=2KB,"
    "intl_node_min=2KB,key_format=u,leaf_node_max=1MB,leaf_node_min=32KB,"
    "runlength_encoding=false,value_format=u";

const char *
__wt_confchk_session_create =
    "allocation_size=(type=int,min=512B,max=128MB),btree_huffman_key=(),"
    "btree_huffman_value=(),btree_key_gap=(type=int,min=0),"
    "btree_internal_key_truncate=(type=boolean),"
    "btree_prefix_compression=(type=boolean),btree_split_min=(type=boolean),"
    "btree_split_pct=(type=int,min=0,max=100),colgroup.name=(),"
    "columns=(type=list),exclusive=(type=boolean),index.name=(),"
    "intl_node_max=(type=int,min=512B,max=512MB),intl_node_min=(type=int,"
    "min=512B,max=512MB),key_format=(type=format),leaf_node_max=(type=int,"
    "min=512B,max=512MB),leaf_node_min=(type=int,min=512B,max=512MB),"
    "runlength_encoding=(type=boolean),value_format=(type=format)";

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
    "printable=false,raw=false";

const char *
__wt_confchk_session_open_cursor =
    "bulk=(type=boolean),dump=(type=boolean),isolation=(choices=[\"snapshot\""
    ",\"read-committed\",\"read-uncommitted\"]),overwrite=(type=boolean),"
    "printable=(type=boolean),raw=(type=boolean)";

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
    "";

const char *
__wt_confchk_session_salvage =
    "";

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
__wt_confdfl_wiredtiger_open =
    "cache_size=20MB,create=false,exclusive=false,extensions=[],error_prefix="
    ",hazard_max=15,logging=false,session_max=50,multiprocess=false,verbose=";

const char *
__wt_confchk_wiredtiger_open =
    "cache_size=(type=int,min=1MB,max=10TB),create=(type=boolean),"
    "exclusive=(type=boolean),extensions=(type=list),error_prefix=(),"
    "hazard_max=(type=int,min=3),logging=(type=boolean),session_max=(type=int"
    ",min=1),multiprocess=(type=boolean),verbose=(type=list,"
    "choices=[\"evict\",\"fileops\",\"hazard\",\"mutex\",\"read\","
    "\"salvage\"])";
