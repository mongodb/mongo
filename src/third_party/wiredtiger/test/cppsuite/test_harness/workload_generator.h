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

#include "core/throttle.h"
#include "workload/database_model.h"
#include "workload/database_operation.h"
#include "workload/random_generator.h"
#include "workload/workload_tracking.h"

namespace test_harness {
/*
 * Class that can execute operations based on a given configuration.
 */
class workload_generator : public component {
    public:
    workload_generator(configuration *configuration, database_operation *db_operation,
      timestamp_manager *timestamp_manager, workload_tracking *tracking)
        : component("workload_generator", configuration), _database_operation(db_operation),
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
    run() override final
    {
        configuration *read_config, *update_config, *insert_config;

        /* Populate the database. */
        _database_operation->populate(_database, _timestamp_manager, _config, _tracking);
        _db_populated = true;

        /* Retrieve useful parameters from the test configuration. */
        update_config = _config->get_subconfig(UPDATE_CONFIG);
        insert_config = _config->get_subconfig(INSERT_CONFIG);
        read_config = _config->get_subconfig(READ_CONFIG);

        /* Generate threads to execute read operations on the collections. */
        for (size_t i = 0; i < read_config->get_int(THREAD_COUNT) && _running; ++i) {
            thread_context *tc =
              new thread_context(read_config, _timestamp_manager, _tracking, _database);
            _workers.push_back(tc);
            _thread_manager.add_thread(
              &database_operation::read_operation, _database_operation, tc);
        }

        /* Generate threads to execute update operations on the collections. */
        for (size_t i = 0; i < update_config->get_int(THREAD_COUNT) && _running; ++i) {
            thread_context *tc =
              new thread_context(update_config, _timestamp_manager, _tracking, _database);
            _workers.push_back(tc);
            _thread_manager.add_thread(
              &database_operation::update_operation, _database_operation, tc);
        }

        delete read_config;
        delete update_config;
        delete insert_config;
    }

    void
    finish() override final
    {
        component::finish();
        for (const auto &it : _workers)
            it->finish();
        _thread_manager.join();
        debug_print("Workload generator: run stage done", DEBUG_TRACE);
    }

    database &
    get_database()
    {
        return (_database);
    }

    bool
    db_populated() const
    {
        return (_db_populated);
    }

    private:
    database _database;
    database_operation *_database_operation;
    thread_manager _thread_manager;
    timestamp_manager *_timestamp_manager;
    workload_tracking *_tracking;
    std::vector<thread_context *> _workers;
    bool _db_populated = false;
};
} // namespace test_harness

#endif
