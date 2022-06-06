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

#include "cache_limit.h"

#include "src/common/logger.h"
#include "src/component/metrics_monitor.h"

extern "C" {
#include "test_util.h"
}

namespace test_harness {

cache_limit::cache_limit(configuration &config, const std::string &name)
    : statistics(config, name, -1)
{
}

void
cache_limit::check(scoped_cursor &cursor)
{
    double use_percent = get_cache_value(cursor);
    if (use_percent > max) {
        const std::string error_string =
          "metrics_monitor: Cache usage exceeded during test! Limit: " + std::to_string(max) +
          " usage: " + std::to_string(use_percent);
        testutil_die(-1, error_string.c_str());
    } else
        logger::log_msg(LOG_TRACE, name + " usage: " + std::to_string(use_percent));
}

std::string
cache_limit::get_value_str(scoped_cursor &cursor)
{
    return std::to_string(get_cache_value(cursor));
}

double
cache_limit::get_cache_value(scoped_cursor &cursor)
{
    int64_t cache_bytes_image, cache_bytes_other, cache_bytes_max;
    double use_percent;
    /* Three statistics are required to compute cache use percentage. */
    metrics_monitor::get_stat(cursor, WT_STAT_CONN_CACHE_BYTES_IMAGE, &cache_bytes_image);
    metrics_monitor::get_stat(cursor, WT_STAT_CONN_CACHE_BYTES_OTHER, &cache_bytes_other);
    metrics_monitor::get_stat(cursor, WT_STAT_CONN_CACHE_BYTES_MAX, &cache_bytes_max);
    /*
     * Assert that we never exceed our configured limit for cache usage. Add 0.0 to avoid floating
     * point conversion errors.
     */
    testutil_assert(cache_bytes_max > 0);
    use_percent = ((cache_bytes_image + cache_bytes_other + 0.0) / cache_bytes_max) * 100;
    return use_percent;
}
} // namespace test_harness
