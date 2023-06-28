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

#include "statistics.h"

#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/component/metrics_monitor.h"

namespace test_harness {

statistics::statistics(configuration &config, const std::string &stat_name, int stat_field)
    : field(stat_field), max(config.get_int(MAX)), min(config.get_int(MIN)), name(stat_name),
      postrun(config.get_bool(POSTRUN_STATISTICS)), runtime(config.get_bool(RUNTIME_STATISTICS)),
      save(config.get_bool(SAVE))
{
}

void
statistics::check(scoped_cursor &cursor)
{
    int64_t stat_value;
    metrics_monitor::get_stat(cursor, field, &stat_value);
    if (stat_value < min || stat_value > max) {
        const std::string error_string = "metrics_monitor: Post-run stat \"" + name +
          "\" was outside of the specified limits. Min=" + std::to_string(min) +
          " Max=" + std::to_string(max) + " Actual=" + std::to_string(stat_value);
        testutil_die(-1, error_string.c_str());
    } else
        logger::log_msg(LOG_TRACE, name + " usage: " + std::to_string(stat_value));
}

std::string
statistics::get_value_str(scoped_cursor &cursor)
{
    int64_t stat_value;
    metrics_monitor::get_stat(cursor, field, &stat_value);
    return std::to_string(stat_value);
}

int
statistics::get_field() const
{
    return field;
}

int64_t
statistics::get_max() const
{
    return max;
}

int64_t
statistics::get_min() const
{
    return min;
}

const std::string &
statistics::get_name() const
{
    return name;
}

bool
statistics::get_postrun() const
{
    return postrun;
}

bool
statistics::get_runtime() const
{
    return runtime;
}

bool
statistics::get_save() const
{
    return save;
}
} // namespace test_harness
