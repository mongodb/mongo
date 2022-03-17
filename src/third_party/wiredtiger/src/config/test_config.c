/* DO NOT EDIT: automatically built by dist/api_config.py. */

#include "wt_internal.h"

static const WT_CONFIG_CHECK confchk_checkpoint_manager_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_stat_cache_size_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0}, {"limit", "int", NULL, "min=0", NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_stat_db_size_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0}, {"limit", "int", NULL, "min=0", NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_runtime_monitor_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {"postrun_statistics", "list", NULL, NULL, NULL, 0},
  {"stat_cache_size", "category", NULL, NULL, confchk_stat_cache_size_subconfigs, 2},
  {"stat_db_size", "category", NULL, NULL, confchk_stat_db_size_subconfigs, 2},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_statistics_config_subconfigs[] = {
  {"enable_logging", "boolean", NULL, NULL, NULL, 0}, {"type", "string", NULL, NULL, NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_timestamp_manager_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0},
  {"oldest_lag", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"op_rate", "string", NULL, NULL, NULL, 0},
  {"stable_lag", "int", NULL, "min=0,max=1000000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_ops_per_transaction_subconfigs[] = {
  {"max", "string", NULL, NULL, NULL, 0}, {"min", "int", NULL, "min=0", NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_insert_config_subconfigs[] = {
  {"key_size", "int", NULL, "min=0,max=10000", NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2},
  {"thread_count", "int", NULL, "min=0", NULL, 0},
  {"value_size", "int", NULL, "min=0,max=1000000000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_populate_config_subconfigs[] = {
  {"collection_count", "int", NULL, "min=0,max=200000", NULL, 0},
  {"key_count_per_collection", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"key_size", "int", NULL, "min=0,max=10000", NULL, 0},
  {"thread_count", "string", NULL, NULL, NULL, 0},
  {"value_size", "int", NULL, "min=0,max=1000000000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_read_config_subconfigs[] = {
  {"key_size", "int", NULL, "min=0,max=10000", NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2},
  {"thread_count", "int", NULL, "min=0", NULL, 0},
  {"value_size", "int", NULL, "min=0,max=1000000000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_update_config_subconfigs[] = {
  {"key_size", "int", NULL, "min=0,max=10000", NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2},
  {"thread_count", "int", NULL, "min=0", NULL, 0},
  {"value_size", "int", NULL, "min=0,max=1000000000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_workload_generator_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0},
  {"insert_config", "category", NULL, NULL, confchk_insert_config_subconfigs, 5},
  {"op_rate", "string", NULL, NULL, NULL, 0},
  {"populate_config", "category", NULL, NULL, confchk_populate_config_subconfigs, 5},
  {"read_config", "category", NULL, NULL, confchk_read_config_subconfigs, 5},
  {"update_config", "category", NULL, NULL, confchk_update_config_subconfigs, 5},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_workload_tracking_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_base_test[] = {
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"checkpoint_manager", "category", NULL, NULL, confchk_checkpoint_manager_subconfigs, 2},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"runtime_monitor", "category", NULL, NULL, confchk_runtime_monitor_subconfigs, 5},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_generator", "category", NULL, NULL, confchk_workload_generator_subconfigs, 6},
  {"workload_tracking", "category", NULL, NULL, confchk_workload_tracking_subconfigs, 2},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_burst_inserts[] = {
  {"burst_duration", "string", NULL, NULL, NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"checkpoint_manager", "category", NULL, NULL, confchk_checkpoint_manager_subconfigs, 2},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"runtime_monitor", "category", NULL, NULL, confchk_runtime_monitor_subconfigs, 5},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_generator", "category", NULL, NULL, confchk_workload_generator_subconfigs, 6},
  {"workload_tracking", "category", NULL, NULL, confchk_workload_tracking_subconfigs, 2},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_example_test[] = {
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"checkpoint_manager", "category", NULL, NULL, confchk_checkpoint_manager_subconfigs, 2},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"runtime_monitor", "category", NULL, NULL, confchk_runtime_monitor_subconfigs, 5},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_generator", "category", NULL, NULL, confchk_workload_generator_subconfigs, 6},
  {"workload_tracking", "category", NULL, NULL, confchk_workload_tracking_subconfigs, 2},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_hs_cleanup[] = {
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"checkpoint_manager", "category", NULL, NULL, confchk_checkpoint_manager_subconfigs, 2},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"runtime_monitor", "category", NULL, NULL, confchk_runtime_monitor_subconfigs, 5},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_generator", "category", NULL, NULL, confchk_workload_generator_subconfigs, 6},
  {"workload_tracking", "category", NULL, NULL, confchk_workload_tracking_subconfigs, 2},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_search_near_01[] = {
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"checkpoint_manager", "category", NULL, NULL, confchk_checkpoint_manager_subconfigs, 2},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"runtime_monitor", "category", NULL, NULL, confchk_runtime_monitor_subconfigs, 5},
  {"search_near_threads", "string", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_generator", "category", NULL, NULL, confchk_workload_generator_subconfigs, 6},
  {"workload_tracking", "category", NULL, NULL, confchk_workload_tracking_subconfigs, 2},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_search_near_02[] = {
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"checkpoint_manager", "category", NULL, NULL, confchk_checkpoint_manager_subconfigs, 2},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"runtime_monitor", "category", NULL, NULL, confchk_runtime_monitor_subconfigs, 5},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_generator", "category", NULL, NULL, confchk_workload_generator_subconfigs, 6},
  {"workload_tracking", "category", NULL, NULL, confchk_workload_tracking_subconfigs, 2},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_ENTRY config_entries[] = {
  {"base_test",
    "cache_size_mb=0,checkpoint_manager=(enabled=false,op_rate=1s),"
    "compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,runtime_monitor=(enabled=true,op_rate=1s,"
    "postrun_statistics=[],stat_cache_size=(enabled=false,limit=0),"
    "stat_db_size=(enabled=false,limit=0)),"
    "statistics_config=(enable_logging=true,type=all),"
    "timestamp_manager=(enabled=true,oldest_lag=1,op_rate=1s,"
    "stable_lag=1),workload_generator=(enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5)),"
    "workload_tracking=(enabled=true,op_rate=1s)",
    confchk_base_test, 10},
  {"burst_inserts",
    "burst_duration=90,cache_size_mb=0,"
    "checkpoint_manager=(enabled=false,op_rate=1s),"
    "compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,runtime_monitor=(enabled=true,op_rate=1s,"
    "postrun_statistics=[],stat_cache_size=(enabled=false,limit=0),"
    "stat_db_size=(enabled=false,limit=0)),"
    "statistics_config=(enable_logging=true,type=all),"
    "timestamp_manager=(enabled=true,oldest_lag=1,op_rate=1s,"
    "stable_lag=1),workload_generator=(enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5)),"
    "workload_tracking=(enabled=true,op_rate=1s)",
    confchk_burst_inserts, 11},
  {"example_test",
    "cache_size_mb=0,checkpoint_manager=(enabled=false,op_rate=1s),"
    "compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,runtime_monitor=(enabled=true,op_rate=1s,"
    "postrun_statistics=[],stat_cache_size=(enabled=false,limit=0),"
    "stat_db_size=(enabled=false,limit=0)),"
    "statistics_config=(enable_logging=true,type=all),"
    "timestamp_manager=(enabled=true,oldest_lag=1,op_rate=1s,"
    "stable_lag=1),workload_generator=(enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5)),"
    "workload_tracking=(enabled=true,op_rate=1s)",
    confchk_example_test, 10},
  {"hs_cleanup",
    "cache_size_mb=0,checkpoint_manager=(enabled=false,op_rate=1s),"
    "compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,runtime_monitor=(enabled=true,op_rate=1s,"
    "postrun_statistics=[],stat_cache_size=(enabled=false,limit=0),"
    "stat_db_size=(enabled=false,limit=0)),"
    "statistics_config=(enable_logging=true,type=all),"
    "timestamp_manager=(enabled=true,oldest_lag=1,op_rate=1s,"
    "stable_lag=1),workload_generator=(enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5)),"
    "workload_tracking=(enabled=true,op_rate=1s)",
    confchk_hs_cleanup, 10},
  {"search_near_01",
    "cache_size_mb=0,checkpoint_manager=(enabled=false,op_rate=1s),"
    "compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,runtime_monitor=(enabled=true,op_rate=1s,"
    "postrun_statistics=[],stat_cache_size=(enabled=false,limit=0),"
    "stat_db_size=(enabled=false,limit=0)),search_near_threads=10,"
    "statistics_config=(enable_logging=true,type=all),"
    "timestamp_manager=(enabled=true,oldest_lag=1,op_rate=1s,"
    "stable_lag=1),workload_generator=(enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5)),"
    "workload_tracking=(enabled=true,op_rate=1s)",
    confchk_search_near_01, 11},
  {"search_near_02",
    "cache_size_mb=0,checkpoint_manager=(enabled=false,op_rate=1s),"
    "compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,runtime_monitor=(enabled=true,op_rate=1s,"
    "postrun_statistics=[],stat_cache_size=(enabled=false,limit=0),"
    "stat_db_size=(enabled=false,limit=0)),"
    "statistics_config=(enable_logging=true,type=all),"
    "timestamp_manager=(enabled=true,oldest_lag=1,op_rate=1s,"
    "stable_lag=1),workload_generator=(enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5)),"
    "workload_tracking=(enabled=true,op_rate=1s)",
    confchk_search_near_02, 10},
  {NULL, NULL, NULL, 0}};

/*
 * __wt_test_config_match --
 *     Return the static configuration entry for a test.
 */
const WT_CONFIG_ENTRY *
__wt_test_config_match(const char *test_name)
{
    const WT_CONFIG_ENTRY *ep;

    for (ep = config_entries; ep->method != NULL; ++ep)
        if (strcmp(test_name, ep->method) == 0)
            return (ep);
    return (NULL);
}
