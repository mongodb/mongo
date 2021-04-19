/* DO NOT EDIT: automatically built by dist/api_config.py. */

#include "wt_internal.h"

static const WT_CONFIG_CHECK confchk_stat_cache_size_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0}, {"limit", "string", NULL, NULL, NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_runtime_monitor_subconfigs[] = {
  {"rate_per_second", "int", NULL, "min=1,max=1000", NULL, 0},
  {"stat_cache_size", "category", NULL, NULL, confchk_stat_cache_size_subconfigs, 2},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_timestamp_manager_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0},
  {"oldest_lag", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"stable_lag", "int", NULL, "min=0,max=1000000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_insert_config_subconfigs[] = {
  {"key_size", "int", NULL, "min=0,max=10000", NULL, 0},
  {"value_size", "int", NULL, "min=0,max=1000000000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_ops_per_transaction_subconfigs[] = {
  {"max", "string", NULL, NULL, NULL, 0}, {"min", "string", NULL, NULL, NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_update_config_subconfigs[] = {
  {"key_size", "int", NULL, "min=0,max=10000", NULL, 0},
  {"value_size", "int", NULL, "min=0,max=1000000000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_workload_generator_subconfigs[] = {
  {"collection_count", "int", NULL, "min=0,max=200000", NULL, 0},
  {"insert_config", "category", NULL, NULL, confchk_insert_config_subconfigs, 2},
  {"insert_threads", "int", NULL, "min=0,max=20", NULL, 0},
  {"key_count", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"key_size", "int", NULL, "min=0,max=10000", NULL, 0},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2},
  {"read_threads", "int", NULL, "min=0,max=100", NULL, 0},
  {"update_config", "category", NULL, NULL, confchk_update_config_subconfigs, 2},
  {"update_threads", "int", NULL, "min=0,max=20", NULL, 0},
  {"value_size", "int", NULL, "min=0,max=1000000000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_workload_tracking_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_example_test[] = {
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"runtime_monitor", "category", NULL, NULL, confchk_runtime_monitor_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 3},
  {"workload_generator", "category", NULL, NULL, confchk_workload_generator_subconfigs, 10},
  {"workload_tracking", "category", NULL, NULL, confchk_workload_tracking_subconfigs, 1},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_poc_test[] = {
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"runtime_monitor", "category", NULL, NULL, confchk_runtime_monitor_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 3},
  {"workload_generator", "category", NULL, NULL, confchk_workload_generator_subconfigs, 10},
  {"workload_tracking", "category", NULL, NULL, confchk_workload_tracking_subconfigs, 1},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_ENTRY config_entries[] = {
  {"example_test",
    "cache_size_mb=0,duration_seconds=0,enable_logging=true,"
    "runtime_monitor=(rate_per_second=1,"
    "stat_cache_size=(enabled=false,limit=)),"
    "timestamp_manager=(enabled=false,oldest_lag=0,stable_lag=0),"
    "workload_generator=(collection_count=1,insert_config=(key_size=0"
    ",value_size=0),insert_threads=0,key_count=0,key_size=0,"
    "ops_per_transaction=(max=1,min=),read_threads=0,"
    "update_config=(key_size=0,value_size=0),update_threads=0,"
    "value_size=0),workload_tracking=(enabled=false)",
    confchk_example_test, 7},
  {"poc_test",
    "cache_size_mb=0,duration_seconds=0,enable_logging=true,"
    "runtime_monitor=(rate_per_second=1,"
    "stat_cache_size=(enabled=false,limit=)),"
    "timestamp_manager=(enabled=false,oldest_lag=0,stable_lag=0),"
    "workload_generator=(collection_count=1,insert_config=(key_size=0"
    ",value_size=0),insert_threads=0,key_count=0,key_size=0,"
    "ops_per_transaction=(max=1,min=),read_threads=0,"
    "update_config=(key_size=0,value_size=0),update_threads=0,"
    "value_size=0),workload_tracking=(enabled=false)",
    confchk_poc_test, 7},
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
