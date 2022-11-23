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

#include "logger.h"

#include <iostream>
#include <sstream>
#include <thread>

extern "C" {
#include "test_util.h"
}

/* Define helpful functions related to debugging. */
namespace test_harness {
/* Order of elements in this array corresponds to the definitions above. */
const char *const LOG_LEVELS[] = {"ERROR", "WARN", "INFO", "TRACE"};

/* Mutex used by logger to synchronize printing. */
static std::mutex _logger_mtx;

/* Set default log level. */
int64_t logger::trace_level = LOG_WARN;

/* Include date in the logs by default. */
bool logger::include_date = true;

void
get_time(char *time_buf, size_t buf_size)
{
    size_t alloc_size;
    struct tm *tm, _tm;

    /* Get time since epoch in nanoseconds. */
    uint64_t epoch_nanosec = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    /* Calculate time since epoch in seconds. */
    time_t time_epoch_sec = epoch_nanosec / WT_BILLION;

    tm = localtime_r(&time_epoch_sec, &_tm);
    testutil_assert(tm != nullptr);

    alloc_size =
      strftime(time_buf, buf_size, logger::include_date ? "[%Y-%m-%dT%H:%M:%S" : "[%H:%M:%S", tm);

    testutil_assert(alloc_size <= buf_size);
    WT_IGNORE_RET(__wt_snprintf(&time_buf[alloc_size], buf_size - alloc_size, ".%" PRIu64 "Z]",
      (uint64_t)epoch_nanosec % WT_BILLION));
}

/* Used to print out traces for debugging purpose. */
void
logger::log_msg(int64_t trace_type, const std::string &str)
{
    if (logger::trace_level >= trace_type) {
        testutil_assert(
          trace_type >= LOG_ERROR && trace_type < sizeof(LOG_LEVELS) / sizeof(LOG_LEVELS[0]));

        char time_buf[64];
        get_time(time_buf, sizeof(time_buf));

        std::ostringstream ss;
        ss << time_buf << "[TID:" << std::this_thread::get_id() << "][" << LOG_LEVELS[trace_type]
           << "]: " << str;

        std::lock_guard<std::mutex> lg(_logger_mtx);
        if (trace_type == LOG_ERROR)
            std::cerr << ss.str() << std::endl;
        else
            std::cout << ss.str() << std::endl;
    }
}
} // namespace test_harness
