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

#ifndef CONN_API_H
#define CONN_API_H

/* Following definitions are required in order to use printing format specifiers in C++. */
#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <mutex>

extern "C" {
#include "test_util.h"
#include "wiredtiger.h"
}

#include "util/scoped_types.h"

namespace test_harness {
/*
 * Singleton class owning the database connection, provides access to sessions and any other
 * required connection API calls.
 */
class connection_manager {
    public:
    static connection_manager &instance();

    public:
    /* No copies of the singleton allowed. */
    connection_manager(connection_manager const &) = delete;
    connection_manager &operator=(connection_manager const &) = delete;

    void close();
    void create(const std::string &config, const std::string &home = DEFAULT_DIR);
    scoped_session create_session();

    WT_CONNECTION *get_connection();

    /*
     * set_timestamp calls into the connection API in a thread safe manner to set global timestamps.
     */
    void set_timestamp(const std::string &config);

    private:
    connection_manager();

    private:
    WT_CONNECTION *_conn = nullptr;
    std::mutex _conn_mutex;
};
} // namespace test_harness

#endif
