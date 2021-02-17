/* DO NOT EDIT: automatically built by dist/api_config.py. */

#include "wt_internal.h"

static const WT_CONFIG_CHECK confchk_poc_test[] = {
  {"collection_count", "int", NULL, "min=0,max=200000", NULL, 0},
  {"insert_config", "string", NULL, NULL, NULL, 0},
  {"insert_threads", "int", NULL, "min=0,max=20", NULL, 0},
  {"key_count", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"key_size", "int", NULL, "min=0,max=10000", NULL, 0},
  {"read_threads", "int", NULL, "min=0,max=100", NULL, 0},
  {"update_config", "string", NULL, NULL, NULL, 0},
  {"update_threads", "int", NULL, "min=0,max=20", NULL, 0},
  {"value_size", "int", NULL, "min=0,max=10000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_ENTRY config_entries[] = {
  {"poc_test",
    "collection_count=1,insert_config=,insert_threads=0,key_count=0,"
    "key_size=0,read_threads=0,update_config=,update_threads=0,"
    "value_size=0",
    confchk_poc_test, 9},
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
