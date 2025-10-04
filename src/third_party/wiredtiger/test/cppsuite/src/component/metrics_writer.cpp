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

#include "metrics_writer.h"

namespace test_harness {
void
metrics_writer::add_stat(const std::string &stat_string)
{
    std::lock_guard<std::mutex> lg(_stat_mutex);
    _stats.push_back(stat_string);
}

void
metrics_writer::output_perf_file(const std::string &test_name)
{
    std::ofstream perf_file(test_name + ".json");
    std::string stat_info = "[{\"info\":{\"test_name\": \"" + test_name + "\"},\"metrics\": [";

    for (const auto &stat : _stats)
        stat_info += stat + ",";

    /* Remove last extra comma. */
    if (stat_info.back() == ',')
        stat_info.pop_back();

    perf_file << stat_info << "]}]";
    perf_file.close();
}

metrics_writer &
metrics_writer::instance()
{
    static metrics_writer _instance;
    return (_instance);
}

metrics_writer::metrics_writer() {}
} // namespace test_harness
