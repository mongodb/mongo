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

#include "database_model.h"
#include "database_operation.h"
#include "random_generator.h"
#include "workload_tracking.h"

namespace test_harness {
/*
 * Class that can execute operations based on a given configuration.
 */
class workload_generator : public component {
    public:
    workload_generator(configuration *configuration, database_operation *db_operation,
      timestamp_manager *timestamp_manager, workload_tracking *tracking)
        : component(configuration), _database_operation(db_operation),
          _timestamp_manager(timestamp_manager), _tracking(tracking)
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

    /* Do the work of the main part of the workload. */
    void
    run()
    {
        configuration *sub_config;
        int64_t min_operation_per_transaction, max_operation_per_transaction, read_threads,
          update_threads, value_size;

        /* Populate the database. */
        _database_operation->populate(_database, _timestamp_manager, _config, _tracking);

        /* Retrieve useful parameters from the test configuration. */
        testutil_check(_config->get_int(READ_THREADS, read_threads));
        testutil_check(_config->get_int(UPDATE_THREADS, update_threads));
        sub_config = _config->get_subconfig(OPS_PER_TRANSACTION);
        testutil_check(sub_config->get_int(MIN, min_operation_per_transaction));
        testutil_check(sub_config->get_int(MAX, max_operation_per_transaction));
        testutil_assert(max_operation_per_transaction >= min_operation_per_transaction);
        testutil_check(_config->get_int(VALUE_SIZE, value_size));
        testutil_assert(value_size >= 0);

        delete sub_config;

        /* Generate threads to execute read operations on the collections. */
        for (int i = 0; i < read_threads; ++i) {
            thread_context *tc =
              new thread_context(_timestamp_manager, _tracking, _database, thread_operation::READ,
                max_operation_per_transaction, min_operation_per_transaction, value_size);
            _workers.push_back(tc);
            _thread_manager.add_thread(tc, _database_operation, &execute_operation);
        }

        /* Generate threads to execute update operations on the collections. */
        for (int i = 0; i < update_threads; ++i) {
            thread_context *tc =
              new thread_context(_timestamp_manager, _tracking, _database, thread_operation::UPDATE,
                max_operation_per_transaction, min_operation_per_transaction, value_size);
            _workers.push_back(tc);
            _thread_manager.add_thread(tc, _database_operation, &execute_operation);
        }
    }

    void
    finish()
    {
        for (const auto &it : _workers)
            it->finish();
        _thread_manager.join();
        debug_print("Workload generator: run stage done", DEBUG_TRACE);
    }

    database &
    get_database()
    {
        return _database;
    }

    /* Workload threaded operations. */
    static void
    execute_operation(thread_context &context, database_operation &db_operation)
    {
        WT_SESSION *session;

        session = connection_manager::instance().create_session();

        switch (context.get_thread_operation()) {
        case thread_operation::READ:
            db_operation.read_operation(context, session);
            break;
        case thread_operation::REMOVE:
        case thread_operation::INSERT:
            /* Sleep until it is implemented. */
            while (context.is_running())
                std::this_thread::sleep_for(std::chrono::seconds(1));
            break;
        case thread_operation::UPDATE:
            db_operation.update_operation(context, session);
            break;
        default:
            testutil_die(DEBUG_ABORT, "system: thread_operation is unknown : %d",
              static_cast<int>(context.get_thread_operation()));
            break;
        }
    }

    private:
    database _database;
    database_operation *_database_operation;
    thread_manager _thread_manager;
    timestamp_manager *_timestamp_manager;
    workload_tracking *_tracking;
    std::vector<thread_context *> _workers;
};
} // namespace test_harness

#endif
