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

#ifndef WORKLOAD_GENERATOR_H
#define WORKLOAD_GENERATOR_H

#include "api_const.h"
#include "random_generator.h"
#include "debug_utils.h"
#include "thread_manager.h"
#include "workload_tracking.h"

namespace test_harness {
/* Class that can execute operations based on a given configuration. */
class workload_generator {
    public:
    workload_generator(configuration *configuration, bool enable_tracking)
        : _configuration(configuration), _enable_tracking(enable_tracking)
    {
    }

    ~workload_generator()
    {
        delete _workload_tracking;

        if (_session != nullptr) {
            if (_session->close(_session, NULL) != 0)
                /* Failing to close session is not blocking. */
                debug_info(
                  "Failed to close session, shutting down uncleanly", _trace_level, DEBUG_ERROR);
            _session = nullptr;
        }

        if (_conn != nullptr) {
            if (_conn->close(_conn, NULL) != 0)
                /* Failing to close connection is not blocking. */
                debug_info(
                  "Failed to close connection, shutting down uncleanly", _trace_level, DEBUG_ERROR);
            _conn = nullptr;
        }
    }

    /*
     * Function that performs the following steps using the configuration that is defined by the
     * test:
     *  - Create the working dir.
     *  - Open a connection.
     *  - Open a session.
     *  - Create n collections as per the configuration.
     *      - Open a cursor on each collection.
     *      - Insert m key/value pairs in each collection. Values are random strings which size is
     * defined by the configuration.
     */
    int
    load(const std::string &home = DEFAULT_DIR)
    {
        WT_CURSOR *cursor;
        int64_t collection_count, key_count, value_size;
        std::string collection_name;

        cursor = nullptr;
        collection_count = key_count = value_size = 0;
        collection_name = "";

        /* Create the working dir. */
        testutil_make_work_dir(home.c_str());

        /* Open connection. */
        testutil_check(wiredtiger_open(home.c_str(), NULL, CONNECTION_CREATE, &_conn));

        /* Create the activity tracker if required. */
        if (_enable_tracking) {
            _workload_tracking = new workload_tracking(_conn, TRACKING_COLLECTION);
            _workload_tracking->load();
        }

        /* Open session. */
        testutil_check(_conn->open_session(_conn, NULL, NULL, &_session));

        /* Create n collections as per the configuration and store each collection name. */
        testutil_check(_configuration->get_int(COLLECTION_COUNT, collection_count));
        for (int i = 0; i < collection_count; ++i) {
            collection_name = "table:collection" + std::to_string(i);
            testutil_check(
              _session->create(_session, collection_name.c_str(), DEFAULT_TABLE_SCHEMA));
            if (_enable_tracking)
                testutil_check(
                  _workload_tracking->save(tracking_operation::CREATE, collection_name, "", ""));
            _collection_names.push_back(collection_name);
        }
        debug_info(
          std::to_string(collection_count) + " collections created", _trace_level, DEBUG_INFO);

        /* Open a cursor on each collection and use the configuration to insert key/value pairs. */
        testutil_check(_configuration->get_int(KEY_COUNT, key_count));
        testutil_check(_configuration->get_int(VALUE_SIZE, value_size));
        for (const auto &collection_name : _collection_names) {
            /* WiredTiger lets you open a cursor on a collection using the same pointer. When a
             * session is closed, WiredTiger APIs close the cursors too. */
            testutil_check(
              _session->open_cursor(_session, collection_name.c_str(), NULL, NULL, &cursor));
            for (size_t j = 0; j < key_count; ++j) {
                /* Generation of a random string value using the size defined in the test
                 * configuration. */
                std::string generated_value =
                  random_generator::random_generator::get_instance().generate_string(value_size);
                testutil_check(
                  insert(cursor, collection_name, j, generated_value.c_str(), _enable_tracking));
            }
        }
        debug_info("Load stage done", _trace_level, DEBUG_INFO);
        return (0);
    }

    /* Do the work of the main part of the workload. */
    int
    run()
    {

        WT_SESSION *session;
        int64_t duration_seconds, read_threads;

        session = nullptr;
        duration_seconds = read_threads = 0;

        testutil_check(_configuration->get_int(DURATION_SECONDS, duration_seconds));
        testutil_check(_configuration->get_int(READ_THREADS, read_threads));

        /* Generate threads to execute read operations on the collections. */
        for (int i = 0; i < read_threads; ++i) {
            testutil_check(_conn->open_session(_conn, NULL, NULL, &session));
            thread_context *tc =
              new thread_context(session, _collection_names, thread_operation::READ);
            _thread_manager.add_thread(tc, &execute_operation);
        }

        /*
         * Spin until duration seconds has expired. If the call to run() returns we destroy the test
         * and the workload generator.
         */
        std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
        _thread_manager.finish();
        debug_info("Run stage done", _trace_level, DEBUG_INFO);
        return 0;
    }

    /* Workload threaded operations. */
    static void
    execute_operation(thread_context &context)
    {
        thread_operation operation;

        operation = context.get_thread_operation();

        if (context.get_session() == nullptr) {
            testutil_die(DEBUG_ABORT, "system: execute_operation : Session is NULL");
        }

        switch (operation) {
        case thread_operation::INSERT:
            /* Sleep until it is implemented. */
            std::this_thread::sleep_for(std::chrono::seconds(1));
            break;
        case thread_operation::READ:
            read_operation(context);
            break;
        case thread_operation::REMOVE:
            /* Sleep until it is implemented. */
            std::this_thread::sleep_for(std::chrono::seconds(1));
            break;
        case thread_operation::UPDATE:
            /* Sleep until it is implemented. */
            std::this_thread::sleep_for(std::chrono::seconds(1));
            break;
        default:
            testutil_die(DEBUG_ABORT, "system: thread_operation is unknown : %d",
              static_cast<int>(thread_operation::UPDATE));
            break;
        }
    }

    /* Basic read operation that walks a cursors across all collections. */
    static void
    read_operation(thread_context &context)
    {
        WT_CURSOR *cursor;
        std::vector<WT_CURSOR *> cursors;

        /* Get a cursor for each collection in collection_names. */
        for (const auto &it : context.get_collection_names()) {
            testutil_check(context.get_session()->open_cursor(
              context.get_session(), it.c_str(), NULL, NULL, &cursor));
            cursors.push_back(cursor);
        }

        while (context.is_running()) {
            /* Walk each cursor. */
            for (const auto &it : cursors)
                it->next(it);
        }
    }

    /* WiredTiger APIs wrappers for single operations. */
    template <typename K, typename V>
    int
    insert(WT_CURSOR *cursor, const std::string &collection_name, K key, V value, bool save)
    {
        int error_code;

        if (cursor == nullptr)
            throw std::invalid_argument("Failed to call insert, invalid cursor");

        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        error_code = cursor->insert(cursor);

        if (error_code == 0) {
            debug_info("key/value inserted", _trace_level, DEBUG_INFO);
            if (save)
                error_code =
                  _workload_tracking->save(tracking_operation::INSERT, collection_name, key, value);
        } else
            debug_info("key/value insertion failed", _trace_level, DEBUG_ERROR);

        return error_code;
    }

    static int
    search(WT_CURSOR *cursor)
    {
        if (cursor == nullptr)
            throw std::invalid_argument("Failed to call search, invalid cursor");
        return (cursor->search(cursor));
    }

    static int
    search_near(WT_CURSOR *cursor, int *exact)
    {
        if (cursor == nullptr)
            throw std::invalid_argument("Failed to call search_near, invalid cursor");
        return (cursor->search_near(cursor, exact));
    }

    static int
    update(WT_CURSOR *cursor)
    {
        if (cursor == nullptr)
            throw std::invalid_argument("Failed to call update, invalid cursor");
        return (cursor->update(cursor));
    }

    private:
    std::vector<std::string> _collection_names;
    configuration *_configuration = nullptr;
    WT_CONNECTION *_conn = nullptr;
    bool _enable_tracking = false;
    WT_SESSION *_session = nullptr;
    thread_manager _thread_manager;
    workload_tracking *_workload_tracking;
};
} // namespace test_harness

#endif
