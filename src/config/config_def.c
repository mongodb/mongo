/* DO NOT EDIT: automatically built by dist/config.py. */

#include "wt_internal.h"

const char *
__wt_confdfl_colgroup_meta =
    "columns=(),filename=""";

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
__wt_confdfl_connection_add_data_source =
    "";

const char *
__wt_confchk_connection_add_data_source =
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
    "entry=wiredtiger_extension_init,prefix=""";

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
__wt_confdfl_file_meta =
    "allocation_size=512B,block_compressor="",checksum=true,collator="","
    "columns=(),huffman_key="",huffman_value="",internal_item_max=0,"
    "internal_key_truncate=true,internal_page_max=2KB,key_format=u,key_gap=10"
    ",leaf_item_max=0,leaf_page_max=1MB,prefix_compression=true,snapshot="","
    "split_pct=75,type=btree,value_format=u,version=(major=0,minor=0)";

const char *
__wt_confchk_file_meta =
    "allocation_size=(type=int,min=512B,max=128MB),block_compressor=(),"
    "checksum=(type=boolean),collator=(),columns=(type=list),huffman_key=(),"
    "huffman_value=(),internal_item_max=(type=int,min=0),"
    "internal_key_truncate=(type=boolean),internal_page_max=(type=int,"
    "min=512B,max=512MB),key_format=(type=format),key_gap=(type=int,min=0),"
    "leaf_item_max=(type=int,min=0),leaf_page_max=(type=int,min=512B,"
    "max=512MB),prefix_compression=(type=boolean),snapshot=(),"
    "split_pct=(type=int,min=25,max=100),type=(choices=[\"btree\"]),"
    "value_format=(type=format),version=()";

const char *
__wt_confdfl_index_meta =
    "columns=(),filename=""";

const char *
__wt_confchk_index_meta =
    "columns=(type=list),filename=()";

const char *
__wt_confdfl_session_begin_transaction =
    "isolation=snapshot,name="",priority=0,sync=full";

const char *
__wt_confchk_session_begin_transaction =
    "isolation=(choices=[\"read-uncommitted\",\"snapshot\"]),name=(),"
    "priority=(type=int,min=-100,max=100),sync=(choices=[\"full\",\"flush\","
    "\"write\",\"none\"])";

const char *
__wt_confdfl_session_checkpoint =
    "drop=(),name="",target=()";

const char *
__wt_confchk_session_checkpoint =
    "drop=(type=list),name=(),target=(type=list)";

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
    "allocation_size=512B,block_compressor="",checksum=true,colgroups=(),"
    "collator="",columns=(),columns=(),exclusive=false,filename="","
    "huffman_key="",huffman_value="",internal_item_max=0,"
    "internal_key_truncate=true,internal_page_max=2KB,key_format=u,"
    "key_format=u,key_gap=10,leaf_item_max=0,leaf_page_max=1MB,"
    "prefix_compression=true,split_pct=75,type=btree,value_format=u,"
    "value_format=u";

const char *
__wt_confchk_session_create =
    "allocation_size=(type=int,min=512B,max=128MB),block_compressor=(),"
    "checksum=(type=boolean),colgroups=(type=list),collator=(),"
    "columns=(type=list),columns=(type=list),exclusive=(type=boolean),"
    "filename=(),huffman_key=(),huffman_value=(),internal_item_max=(type=int,"
    "min=0),internal_key_truncate=(type=boolean),internal_page_max=(type=int,"
    "min=512B,max=512MB),key_format=(type=format),key_format=(type=format),"
    "key_gap=(type=int,min=0),leaf_item_max=(type=int,min=0),"
    "leaf_page_max=(type=int,min=512B,max=512MB),"
    "prefix_compression=(type=boolean),split_pct=(type=int,min=25,max=100),"
    "type=(choices=[\"btree\"]),value_format=(type=format),"
    "value_format=(type=format)";

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
    "append=false,bulk=false,checkpoint="",dump="",isolation=read-committed,"
    "overwrite=false,raw=false,statistics=false,statistics_clear=false";

const char *
__wt_confchk_session_open_cursor =
    "append=(type=boolean),bulk=(type=boolean),checkpoint=(),"
    "dump=(choices=[\"hex\",\"print\"]),isolation=(choices=[\"snapshot\","
    "\"read-committed\",\"read-uncommitted\"]),overwrite=(type=boolean),"
    "raw=(type=boolean),statistics=(type=boolean),"
    "statistics_clear=(type=boolean)";

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
__wt_confdfl_session_truncate =
    "";

const char *
__wt_confchk_session_truncate =
    "";

const char *
__wt_confdfl_session_upgrade =
    "";

const char *
__wt_confchk_session_upgrade =
    "";

const char *
__wt_confdfl_session_verify =
    "";

const char *
__wt_confchk_session_verify =
    "";

const char *
__wt_confdfl_table_meta =
    "colgroups=(),columns=(),key_format=u,value_format=u";

const char *
__wt_confchk_table_meta =
    "colgroups=(type=list),columns=(type=list),key_format=(type=format),"
    "value_format=(type=format)";

const char *
__wt_confdfl_wiredtiger_open =
    "buffer_alignment=-1,cache_size=100MB,create=false,direct_io=(),"
    "error_prefix="",eviction_target=80,eviction_trigger=95,extensions=(),"
    "hazard_max=30,logging=false,multiprocess=false,session_max=50,sync=true,"
    "transactional=true,use_environment_priv=false,verbose=()";

const char *
__wt_confchk_wiredtiger_open =
    "buffer_alignment=(type=int,min=-1,max=1MB),cache_size=(type=int,min=1MB,"
    "max=10TB),create=(type=boolean),direct_io=(type=list,choices=[\"data\","
    "\"log\"]),error_prefix=(),eviction_target=(type=int,min=10,max=99),"
    "eviction_trigger=(type=int,min=10,max=99),extensions=(type=list),"
    "hazard_max=(type=int,min=15),logging=(type=boolean),"
    "multiprocess=(type=boolean),session_max=(type=int,min=1),"
    "sync=(type=boolean),transactional=(type=boolean),"
    "use_environment_priv=(type=boolean),verbose=(type=list,"
    "choices=[\"block\",\"evict\",\"evictserver\",\"fileops\",\"hazard\","
    "\"mutex\",\"read\",\"readserver\",\"reconcile\",\"salvage\",\"snapshot\""
    ",\"verify\",\"write\"])";
