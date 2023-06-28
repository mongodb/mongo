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

#ifndef METRICS_WRITER_H
#define METRICS_WRITER_H

#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace test_harness {
/* Singleton class that can write statistics to a file. */
class metrics_writer {
public:
    static metrics_writer &instance();

public:
    /* No copies of the singleton allowed. */
    metrics_writer(metrics_writer const &) = delete;
    metrics_writer &operator=(metrics_writer const &) = delete;

    void add_stat(const std::string &stat_string);
    void output_perf_file(const std::string &test_name);

private:
    metrics_writer();
    std::vector<std::string> _stats;
    std::mutex _stat_mutex;
};
} // namespace test_harness

#endif
