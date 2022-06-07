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

/*
 * This file provides an example of how to create a test in C++ using a few features from the
 * framework if any. This file can be used as a template for quick testing and/or when stress
 * testing is not required. For any stress testing, it is encouraged to use the framework, see
 * test_template.cpp and create_script.sh.
 */

#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/common/random_generator.h"
#include "src/common/thread_manager.h"
#include "src/storage/connection_manager.h"

extern "C" {
#include "wiredtiger.h"
#include "test_util.h"
}

using namespace test_harness;

/* Declarations to avoid the error raised by -Werror=missing-prototypes. */
void insert_op(WT_CURSOR *cursor, int key_size, int value_size);
void read_op(WT_CURSOR *cursor, int key_size);

bool do_inserts = false;
bool do_reads = false;

void
insert_op(WT_CURSOR *cursor, int key_size, int value_size)
{
    logger::log_msg(LOG_INFO, "called insert_op");

    /* Insert random data. */
    std::string key, value;
    while (do_inserts) {
        key = random_generator::instance().generate_random_string(key_size);
        value = random_generator::instance().generate_random_string(value_size);
        cursor->set_key(cursor, key.c_str());
        cursor->set_value(cursor, value.c_str());
        testutil_check(cursor->insert(cursor));
    }
}

void
read_op(WT_CURSOR *cursor, int key_size)
{
    logger::log_msg(LOG_INFO, "called read_op");

    /* Read random data. */
    std::string key;
    while (do_reads) {
        key = random_generator::instance().generate_random_string(key_size);
        cursor->set_key(cursor, key.c_str());
        WT_IGNORE_RET(cursor->search(cursor));
    }
}

int
main(int argc, char *argv[])
{
    /* Set the program name for error messages. */
    const std::string progname = testutil_set_progname(argv);

    /* Set the tracing level for the logger component. */
    logger::trace_level = LOG_INFO;

    /* Printing some messages. */
    logger::log_msg(LOG_INFO, "Starting " + progname);
    logger::log_msg(LOG_ERROR, "This could be an error.");

    /* Create a connection, set the cache size and specify the home directory. */
    const std::string conn_config = CONNECTION_CREATE + ",cache_size=500MB";
    const std::string home_dir = std::string(DEFAULT_DIR) + '_' + progname;

    /* Create connection. */
    connection_manager::instance().create(conn_config, home_dir);
    WT_CONNECTION *conn = connection_manager::instance().get_connection();

    /* Open different sessions. */
    WT_SESSION *insert_session, *read_session;
    testutil_check(conn->open_session(conn, nullptr, nullptr, &insert_session));
    testutil_check(conn->open_session(conn, nullptr, nullptr, &read_session));

    /* Create a collection. */
    const std::string collection_name = "table:my_collection";
    testutil_check(insert_session->create(
      insert_session, collection_name.c_str(), DEFAULT_FRAMEWORK_SCHEMA.c_str()));

    /* Open different cursors. */
    WT_CURSOR *insert_cursor, *read_cursor;
    const std::string cursor_config = "";
    testutil_check(insert_session->open_cursor(
      insert_session, collection_name.c_str(), nullptr, cursor_config.c_str(), &insert_cursor));
    testutil_check(read_session->open_cursor(
      read_session, collection_name.c_str(), nullptr, cursor_config.c_str(), &read_cursor));

    /* Store cursors. */
    std::vector<WT_CURSOR *> cursors;
    cursors.push_back(insert_cursor);
    cursors.push_back(read_cursor);

    /* Insert some data. */
    std::string key = "a";
    const std::string value = "b";
    insert_cursor->set_key(insert_cursor, key.c_str());
    insert_cursor->set_value(insert_cursor, value.c_str());
    testutil_check(insert_cursor->insert(insert_cursor));

    /* Read some data. */
    key = "b";
    read_cursor->set_key(read_cursor, key.c_str());
    testutil_assert(read_cursor->search(read_cursor) == WT_NOTFOUND);

    key = "a";
    read_cursor->set_key(read_cursor, key.c_str());
    testutil_check(read_cursor->search(read_cursor));

    /* Create a thread manager and spawn some threads that will work. */
    thread_manager t;
    int key_size = 1, value_size = 2;

    do_inserts = true;
    t.add_thread(insert_op, insert_cursor, key_size, value_size);

    do_reads = true;
    t.add_thread(read_op, read_cursor, key_size);

    /* Sleep for the test duration. */
    int test_duration_s = 5;
    std::this_thread::sleep_for(std::chrono::seconds(test_duration_s));

    /* Stop the threads. */
    do_reads = false;
    do_inserts = false;
    t.join();

    /* Close cursors. */
    for (auto c : cursors)
        testutil_check(c->close(c));

    /* Another message. */
    logger::log_msg(LOG_INFO, "End of test.");

    return (0);
}
