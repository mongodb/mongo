/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "test_util.h"

/*
 * testutil_disagg_storage_configuration --
 *     Set up disagg storage configuration.
 */
void
testutil_disagg_storage_configuration(TEST_OPTS *opts, const char *home, char *disagg_cfg,
  size_t disagg_cfg_size, char *ext_cfg, size_t ext_cfg_size)
{
    (void)home;

    if (opts->disagg_storage) {
        testutil_snprintf(ext_cfg, ext_cfg_size, TESTUTIL_ENV_CONFIG_DISAGG_EXT, opts->build_dir,
          opts->disagg_page_log, opts->disagg_page_log, opts->delay_ms, opts->error_ms,
          opts->force_delay, opts->force_error, opts->palm_map_size_mb);

        testutil_snprintf(disagg_cfg, disagg_cfg_size, TESTUTIL_ENV_CONFIG_DISAGG,
          opts->disagg_mode, opts->disagg_page_log);
    } else {
        testutil_snprintf(ext_cfg, ext_cfg_size, "\"\"");
        testutil_assert(disagg_cfg_size > 0);
        disagg_cfg[0] = '\0';
    }
}
