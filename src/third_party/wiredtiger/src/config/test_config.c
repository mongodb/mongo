/* DO NOT EDIT: automatically built by dist/api_config.py. */

#include "wt_internal.h"

static const WT_CONFIG_CHECK confchk_stat_cache_size_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0}, {"limit", "string", NULL, NULL, NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_poc_test[] = {
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"collection_count", "int", NULL, "min=0,max=200000", NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"enable_timestamp", "boolean", NULL, NULL, NULL, 0},
  {"enable_tracking", "boolean", NULL, NULL, NULL, 0},
  {"insert_config", "string", NULL, NULL, NULL, 0},
  {"insert_threads", "int", NULL, "min=0,max=20", NULL, 0},
  {"key_count", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"key_size", "int", NULL, "min=0,max=10000", NULL, 0},
  {"max_operation_per_transaction", "int", NULL, "min=1,max=200000", NULL, 0},
  {"min_operation_per_transaction", "int", NULL, "min=1,max=200000", NULL, 0},
  {"oldest_lag", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"rate_per_second", "int", NULL, "min=1,max=1000", NULL, 0},
  {"read_threads", "int", NULL, "min=0,max=100", NULL, 0},
  {"stable_lag", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"stat_cache_size", "category", NULL, NULL, confchk_stat_cache_size_subconfigs, 2},
  {"update_config", "string", NULL, NULL, NULL, 0},
  {"update_threads", "int", NULL, "min=0,max=20", NULL, 0},
  {"value_size", "int", NULL, "min=0,max=1000000000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_ENTRY config_entries[] = {
  {"poc_test",
    "cache_size_mb=0,collection_count=1,duration_seconds=0,"
    "enable_logging=true,enable_timestamp=true,enable_tracking=true,"
    "insert_config=,insert_threads=0,key_count=0,key_size=0,"
    "max_operation_per_transaction=1,min_operation_per_transaction=1,"
    "oldest_lag=0,rate_per_second=1,read_threads=0,stable_lag=0,"
    "stat_cache_size=(enabled=false,limit=),update_config=,"
    "update_threads=0,value_size=0",
    confchk_poc_test, 20},
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
