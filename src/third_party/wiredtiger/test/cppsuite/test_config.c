/* DO NOT EDIT: automatically built by dist/api_config.py. */

#include "wt_internal.h"

static const WT_CONFIG_CHECK confchk_poc_test[] = {
  {"collection_count", "int", NULL, "min=1,max=10", NULL, 0},
  {"key_size", "int", NULL, "min=1,max=10000", NULL, 0},
  {"values", "string", NULL, "choices=[\"first\",\"second\",\"third\"]", NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_ENTRY config_entries[] = {
  {"poc_test", "collection_count=1,key_size=10,values=first", confchk_poc_test, 3},
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
