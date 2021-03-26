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

#include <algorithm>
#include <atomic>
#include <map>

#include "random_generator.h"
#include "workload_tracking.h"

namespace test_harness {
/*
 * Class that can execute operations based on a given configuration.
 */
class workload_generator : public component {
    public:
    workload_generator(configuration *configuration, timestamp_manager *timestamp_manager,
      workload_tracking *tracking)
        : component(configuration), _timestamp_manager(timestamp_manager), _tracking(tracking)
    {
    }

    ~workload_generator()
    {
        for (auto &it : _workers)
            delete it;
    }

    /* Delete the copy constructor and the assignment operator. */
    workload_generator(const workload_generator &) = delete;
    workload_generator &operator=(const workload_generator &) = delete;

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
    void
    populate()
    {
        WT_CURSOR *cursor;
        WT_SESSION *session;
        wt_timestamp_t ts;
        int64_t collection_count, key_count, value_size;
        std::string collection_name, config, generated_value, home;
        bool ts_enabled = _timestamp_manager->is_enabled();

        cursor = nullptr;
        collection_count = key_count = value_size = 0;
        collection_name = "";

        /* Get a session. */
        session = connection_manager::instance().create_session();
        /* Create n collections as per the configuration and store each collection name. */
        testutil_check(_config->get_int(COLLECTION_COUNT, collection_count));
        for (int i = 0; i < collection_count; ++i) {
            collection_name = "table:collection" + std::to_string(i);
            testutil_check(session->create(session, collection_name.c_str(), DEFAULT_TABLE_SCHEMA));
            ts = _timestamp_manager->get_next_ts();
            testutil_check(_tracking->save(tracking_operation::CREATE, collection_name, 0, "", ts));
            _collection_names.push_back(collection_name);
        }
        debug_print(std::to_string(collection_count) + " collections created", DEBUG_TRACE);

        /* Open a cursor on each collection and use the configuration to insert key/value pairs. */
        testutil_check(_config->get_int(KEY_COUNT, key_count));
        testutil_check(_config->get_int(VALUE_SIZE, value_size));
        testutil_assert(value_size >= 0);
        for (const auto &collection_name : _collection_names) {
            /* WiredTiger lets you open a cursor on a collection using the same pointer. When a
             * session is closed, WiredTiger APIs close the cursors too. */
            testutil_check(
              session->open_cursor(session, collection_name.c_str(), NULL, NULL, &cursor));
            for (size_t j = 0; j < key_count; ++j) {
                /*
                 * Generation of a random string value using the size defined in the test
                 * configuration.
                 */
                generated_value =
                  random_generator::random_generator::instance().generate_string(value_size);
                ts = _timestamp_manager->get_next_ts();
                if (ts_enabled)
                    testutil_check(session->begin_transaction(session, ""));
                testutil_check(insert(cursor, collection_name, j + 1, generated_value.c_str(), ts));
                if (ts_enabled) {
                    config = std::string(COMMIT_TS) + "=" + _timestamp_manager->decimal_to_hex(ts);
                    testutil_check(session->commit_transaction(session, config.c_str()));
                }
            }
        }
        debug_print("Populate stage done", DEBUG_TRACE);
    }

    /* Do the work of the main part of the workload. */
    void
    run()
    {
        configuration *sub_config;
        int64_t read_threads, min_operation_per_transaction, max_operation_per_transaction,
          value_size;

        /* Populate the database. */
        populate();

        /* Retrieve useful parameters from the test configuration. */
        testutil_check(_config->get_int(READ_THREADS, read_threads));
        sub_config = _config->get_subconfig(OPS_PER_TRANSACTION);
        testutil_check(sub_config->get_int(MIN, min_operation_per_transaction));
        testutil_check(sub_config->get_int(MAX, max_operation_per_transaction));
        testutil_assert(max_operation_per_transaction >= min_operation_per_transaction);
        testutil_check(_config->get_int(VALUE_SIZE, value_size));
        testutil_assert(value_size >= 0);

        delete sub_config;

        /* Generate threads to execute read operations on the collections. */
        for (int i = 0; i < read_threads; ++i) {
            thread_context *tc = new thread_context(_timestamp_manager, _tracking,
              _collection_names, thread_operation::READ, max_operation_per_transaction,
              min_operation_per_transaction, value_size);
            _workers.push_back(tc);
            _thread_manager.add_thread(tc, &execute_operation);
        }
    }

    void
    finish()
    {
        for (const auto &it : _workers) {
            it->finish();
        }
        _thread_manager.join();
        debug_print("Workload generator: run stage done", DEBUG_TRACE);
    }

    /* Workload threaded operations. */
    static void
    execute_operation(thread_context &context)
    {
        WT_SESSION *session;

        session = connection_manager::instance().create_session();

        switch (context.get_thread_operation()) {
        case thread_operation::READ:
            read_operation(context, session);
            break;
        case thread_operation::REMOVE:
        case thread_operation::INSERT:
            /* Sleep until it is implemented. */
            while (context.is_running())
                std::this_thread::sleep_for(std::chrono::seconds(1));
            break;
        case thread_operation::UPDATE:
            update_operation(context, session);
            break;
        default:
            testutil_die(DEBUG_ABORT, "system: thread_operation is unknown : %d",
              static_cast<int>(context.get_thread_operation()));
            break;
        }
    }

    /*
     * Basic update operation that currently update the same key with a random value in each
     * collection.
     */
    static void
    update_operation(thread_context &context, WT_SESSION *session)
    {
        WT_CURSOR *cursor;
        wt_timestamp_t ts;
        std::vector<WT_CURSOR *> cursors;
        std::vector<std::string> collection_names;
        std::string generated_value;
        bool has_committed = true;
        int64_t cpt, value_size = context.get_value_size();

        testutil_assert(session != nullptr);
        /* Get a cursor for each collection in collection_names. */
        for (const auto &it : context.get_collection_names()) {
            testutil_check(session->open_cursor(session, it.c_str(), NULL, NULL, &cursor));
            cursors.push_back(cursor);
            collection_names.push_back(it);
        }

        while (context.is_running()) {
            /* Walk each cursor. */
            context.begin_transaction(session, "");
            ts = context.set_commit_timestamp(session);
            cpt = 0;
            for (const auto &it : cursors) {
                generated_value =
                  random_generator::random_generator::instance().generate_string(value_size);
                /* Key is hard coded for now. */
                testutil_check(update(context.get_tracking(), it, collection_names[cpt], 1,
                  generated_value.c_str(), ts));
                ++cpt;
            }
            has_committed = context.commit_transaction(session, "");
        }

        /* Make sure the last operation is committed now the work is finished. */
        if (!has_committed)
            context.commit_transaction(session, "");
    }

    /* Basic read operation that walks a cursors across all collections. */
    static void
    read_operation(thread_context &context, WT_SESSION *session)
    {
        WT_CURSOR *cursor;
        std::vector<WT_CURSOR *> cursors;

        testutil_assert(session != nullptr);
        /* Get a cursor for each collection in collection_names. */
        for (const auto &it : context.get_collection_names()) {
            testutil_check(session->open_cursor(session, it.c_str(), NULL, NULL, &cursor));
            cursors.push_back(cursor);
        }

        while (context.is_running()) {
            /* Walk each cursor. */
            for (const auto &it : cursors) {
                if (it->next(it) != 0)
                    it->reset(it);
            }
        }
    }

    /* WiredTiger APIs wrappers for single operations. */
    template <typename K, typename V>
    int
    insert(WT_CURSOR *cursor, const std::string &collection_name, K key, V value, wt_timestamp_t ts)
    {
        int error_code;

        testutil_assert(cursor != nullptr);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        error_code = cursor->insert(cursor);

        if (error_code == 0) {
            debug_print("key/value inserted", DEBUG_TRACE);
            error_code =
              _tracking->save(tracking_operation::INSERT, collection_name, key, value, ts);
        } else
            debug_print("key/value insertion failed", DEBUG_ERROR);

        return (error_code);
    }

    static int
    search(WT_CURSOR *cursor)
    {
        testutil_assert(cursor != nullptr);
        return (cursor->search(cursor));
    }

    static int
    search_near(WT_CURSOR *cursor, int *exact)
    {
        testutil_assert(cursor != nullptr);
        return (cursor->search_near(cursor, exact));
    }

    template <typename K, typename V>
    static int
    update(workload_tracking *tracking, WT_CURSOR *cursor, const std::string &collection_name,
      K key, V value, wt_timestamp_t ts)
    {
        int error_code;

        testutil_assert(tracking != nullptr);
        testutil_assert(cursor != nullptr);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        error_code = cursor->update(cursor);

        if (error_code == 0) {
            debug_print("key/value update", DEBUG_TRACE);
            error_code =
              tracking->save(tracking_operation::UPDATE, collection_name, key, value, ts);
        } else
            debug_print("key/value update failed", DEBUG_ERROR);

        return (error_code);
    }

    private:
    std::vector<std::string> _collection_names;
    thread_manager _thread_manager;
    timestamp_manager *_timestamp_manager;
    workload_tracking *_tracking;
    std::vector<thread_context *> _workers;
};
} // namespace test_harness

#endif
