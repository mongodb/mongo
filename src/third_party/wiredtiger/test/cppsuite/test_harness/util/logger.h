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

/* Following definitions are required in order to use printing format specifiers in C++. */
#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

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

void get_time(char *time_buf, size_t buf_size);

class logger {
    public:
    /* Current log level. Default is LOG_WARN. */
    static int64_t trace_level;

    /* Include date in the logs if enabled. Default is true. */
    static bool include_date;

    public:
    /* Used to print out traces for debugging purpose. */
    static void log_msg(int64_t trace_type, const std::string &str);

    /* Make sure the class will not be instantiated. */
    logger() = delete;
};
} // namespace test_harness

#endif
