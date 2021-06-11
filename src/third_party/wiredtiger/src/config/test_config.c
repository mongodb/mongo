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
  {"stat_cache_size", "category", NULL, NULL, confchk_stat_cache_size_subconfigs, 2},
  {"stat_db_size", "category", NULL, NULL, confchk_stat_db_size_subconfigs, 2},
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
  {"thread_count", "string", NULL, NULL, NULL, 0},
  {"value_size", "int", NULL, "min=0,max=1000000000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_read_config_subconfigs[] = {
  {"op_rate", "string", NULL, NULL, NULL, 0},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2},
  {"thread_count", "string", NULL, NULL, NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_update_config_subconfigs[] = {
  {"key_size", "int", NULL, "min=0,max=10000", NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2},
  {"thread_count", "string", NULL, NULL, NULL, 0},
  {"value_size", "int", NULL, "min=0,max=1000000000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_workload_generator_subconfigs[] = {
  {"collection_count", "int", NULL, "min=0,max=200000", NULL, 0},
  {"enabled", "boolean", NULL, NULL, NULL, 0},
  {"insert_config", "category", NULL, NULL, confchk_insert_config_subconfigs, 5},
  {"key_count", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"key_size", "int", NULL, "min=0,max=10000", NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {"read_config", "category", NULL, NULL, confchk_read_config_subconfigs, 3},
  {"update_config", "category", NULL, NULL, confchk_update_config_subconfigs, 5},
  {"value_size", "int", NULL, "min=0,max=1000000000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_workload_tracking_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_example_test[] = {
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"checkpoint_manager", "category", NULL, NULL, confchk_checkpoint_manager_subconfigs, 2},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"runtime_monitor", "category", NULL, NULL, confchk_runtime_monitor_subconfigs, 4},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_generator", "category", NULL, NULL, confchk_workload_generator_subconfigs, 9},
  {"workload_tracking", "category", NULL, NULL, confchk_workload_tracking_subconfigs, 2},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_poc_test[] = {
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"checkpoint_manager", "category", NULL, NULL, confchk_checkpoint_manager_subconfigs, 2},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"runtime_monitor", "category", NULL, NULL, confchk_runtime_monitor_subconfigs, 4},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_generator", "category", NULL, NULL, confchk_workload_generator_subconfigs, 9},
  {"workload_tracking", "category", NULL, NULL, confchk_workload_tracking_subconfigs, 2},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_ENTRY config_entries[] = {
  {"example_test",
    "cache_size_mb=0,checkpoint_manager=(enabled=false,op_rate=1s),"
    "duration_seconds=0,enable_logging=false,"
    "runtime_monitor=(enabled=true,op_rate=1s,"
    "stat_cache_size=(enabled=false,limit=0),"
    "stat_db_size=(enabled=false,limit=0)),"
    "timestamp_manager=(enabled=true,oldest_lag=1,op_rate=1s,"
    "stable_lag=1),workload_generator=(collection_count=1,"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=1,value_size=5),"
    "key_count=0,key_size=5,op_rate=1s,read_config=(op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=1),"
    "update_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=1,value_size=5),value_size=5),"
    "workload_tracking=(enabled=true,op_rate=1s)",
    confchk_example_test, 8},
  {"poc_test",
    "cache_size_mb=0,checkpoint_manager=(enabled=false,op_rate=1s),"
    "duration_seconds=0,enable_logging=false,"
    "runtime_monitor=(enabled=true,op_rate=1s,"
    "stat_cache_size=(enabled=false,limit=0),"
    "stat_db_size=(enabled=false,limit=0)),"
    "timestamp_manager=(enabled=true,oldest_lag=1,op_rate=1s,"
    "stable_lag=1),workload_generator=(collection_count=1,"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=1,value_size=5),"
    "key_count=0,key_size=5,op_rate=1s,read_config=(op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=1),"
    "update_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=1,value_size=5),value_size=5),"
    "workload_tracking=(enabled=true,op_rate=1s)",
    confchk_poc_test, 8},
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
