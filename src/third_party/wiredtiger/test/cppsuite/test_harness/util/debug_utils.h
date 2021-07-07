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

#ifndef DEBUG_UTILS_H
#define DEBUG_UTILS_H

#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

extern "C" {
#include "test_util.h"
}

/* Define helpful functions related to debugging. */
namespace test_harness {

/* Possible log levels. If you change anything here make sure you update LOG_LEVELS accordingly. */
#define LOG_ERROR 0
#define LOG_WARN 1
#define LOG_INFO 2
/*
 * The trace log level can incur a performance overhead since the current logging implementation
 * requires per-line locking.
 */
#define LOG_TRACE 3

/* Order of elements in this array corresponds to the definitions above. */
const char *const LOG_LEVELS[] = {"ERROR", "WARN", "INFO", "TRACE"};

/* Current log level */
static int64_t _trace_level = LOG_WARN;

/* Include date in the logs if enabled. */
static bool _include_date = true;

/* Mutex used by logger to synchronize printing. */
extern std::mutex _logger_mtx;

void
get_time(char *time_buf, size_t buf_size)
{
    size_t alloc_size;
    struct timespec tsp;
    struct tm *tm, _tm;

    /* Get time since epoch in nanoseconds. */
    uint64_t epoch_nanosec = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    /* Calculate time since epoch in seconds. */
    time_t time_epoch_sec = epoch_nanosec / WT_BILLION;

    tm = localtime_r(&time_epoch_sec, &_tm);

    alloc_size =
      strftime(time_buf, buf_size, _include_date ? "[%Y-%m-%dT%H:%M:%S" : "[%H:%M:%S", tm);

    testutil_assert(alloc_size <= buf_size);
    WT_IGNORE_RET(__wt_snprintf(&time_buf[alloc_size], buf_size - alloc_size, ".%" PRIu64 "Z]",
      (uint64_t)epoch_nanosec % WT_BILLION));
}

/* Used to print out traces for debugging purpose. */
static void
log_msg(int64_t trace_type, const std::string &str)
{
    if (_trace_level >= trace_type) {
        testutil_assert(
          trace_type >= LOG_ERROR && trace_type < sizeof(LOG_LEVELS) / sizeof(LOG_LEVELS[0]));

        char time_buf[64];
        get_time(time_buf, sizeof(time_buf));

        std::ostringstream ss;
        ss << time_buf << "[TID:" << std::this_thread::get_id() << "][" << LOG_LEVELS[trace_type]
           << "]: " << str << std::endl;

        std::lock_guard<std::mutex> lg(_logger_mtx);
        if (trace_type == LOG_ERROR)
            std::cerr << ss.str();
        else
            std::cout << ss.str();
    }
}
} // namespace test_harness

#endif
