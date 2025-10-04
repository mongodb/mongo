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

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "src/main/test.h"

namespace test_harness {

/*
 * Class that measures the average execution time of a given function and adds the stats to the
 * statistics writer when destroyed.
 */
class execution_timer {
public:
    execution_timer(const std::string &id, const std::string &test_name);
    virtual ~execution_timer();

    /* Calculates the average time and appends the stat to the perf file. */
    void append_stats();

    /*
     * Counts hardware instructions used for a given operation and keeps track of how many
     * operations have been executed.
     */
    template <typename T>
    int
    track(T lambda)
    {
        auto start_time = std::chrono::steady_clock::now();

        int ret = lambda();
        _time_recordings.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
          (std::chrono::steady_clock::now() - start_time))
                                     .count());
        return ret;
    }

private:
    std::string _id;
    std::string _test_name;
    std::vector<uint64_t> _time_recordings;
};
} // namespace test_harness
