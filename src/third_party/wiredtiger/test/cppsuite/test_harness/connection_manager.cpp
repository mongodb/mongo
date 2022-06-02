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

#include "connection_manager.h"
#include "util/api_const.h"
#include "util/logger.h"
#include "util/scoped_connection.h"

namespace test_harness {
connection_manager &
connection_manager::instance()
{
    static connection_manager _instance;
    return (_instance);
}

void
connection_manager::close()
{
    if (_conn != nullptr) {
        testutil_check(_conn->close(_conn, nullptr));
        _conn = nullptr;
    }
}

void
connection_manager::create(const std::string &config, const std::string &home)
{
    if (_conn != nullptr) {
        logger::log_msg(LOG_ERROR, "Connection is not NULL, cannot be re-opened.");
        testutil_die(EINVAL, "Connection is not NULL");
    }
    logger::log_msg(LOG_INFO, "wiredtiger_open config: " + config);

    /* Create the working dir. */
    testutil_make_work_dir(home.c_str());

    /* Open conn. */
    testutil_check(wiredtiger_open(home.c_str(), nullptr, config.c_str(), &_conn));
}

scoped_session
connection_manager::create_session()
{
    if (_conn == nullptr) {
        logger::log_msg(LOG_ERROR,
          "Connection is NULL, did you forget to call "
          "connection_manager::create ?");
        testutil_die(EINVAL, "Connection is NULL");
    }

    std::lock_guard<std::mutex> lg(_conn_mutex);
    scoped_session session(_conn);

    return (session);
}

WT_CONNECTION *
connection_manager::get_connection()
{
    return (_conn);
}

/*
 * set_timestamp calls into the connection API in a thread safe manner to set global timestamps.
 */
void
connection_manager::set_timestamp(const std::string &config)
{
    std::lock_guard<std::mutex> lg(_conn_mutex);
    testutil_check(_conn->set_timestamp(_conn, config.c_str()));
}

connection_manager::connection_manager() {}
} // namespace test_harness
