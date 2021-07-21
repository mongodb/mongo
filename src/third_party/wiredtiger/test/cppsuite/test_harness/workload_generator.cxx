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

#include <atomic>
#include <map>

#include "connection_manager.h"
#include "core/configuration.h"
#include "core/throttle.h"
#include "util/api_const.h"
#include "workload/database_model.h"
#include "workload/database_operation.h"
#include "workload/random_generator.h"
#include "workload/workload_tracking.h"
#include "workload_generator.h"

namespace test_harness {
/* operation_config class implementation */
operation_config::operation_config(configuration *config, thread_type type)
    : config(config), type(type), thread_count(config->get_int(THREAD_COUNT))
{
}

std::function<void(test_harness::thread_context *)>
operation_config::get_func(database_operation *dbo)
{
    switch (type) {
    case thread_type::INSERT:
        return (std::bind(&database_operation::insert_operation, dbo, std::placeholders::_1));
    case thread_type::READ:
        return (std::bind(&database_operation::read_operation, dbo, std::placeholders::_1));
    case thread_type::UPDATE:
        return (std::bind(&database_operation::update_operation, dbo, std::placeholders::_1));
    default:
        /* This may cause a separate testutil_die in type_string but that should be okay. */
        testutil_die(EINVAL, "unexpected thread_type: %s", type_string(type).c_str());
    }
}

/* workload_generator class implementation */
workload_generator::workload_generator(configuration *configuration,
  database_operation *db_operation, timestamp_manager *timestamp_manager,
  workload_tracking *tracking, database &database)
    : component("workload_generator", configuration), _database(database),
      _database_operation(db_operation), _timestamp_manager(timestamp_manager), _tracking(tracking)
{
}

workload_generator::~workload_generator()
{
    for (auto &it : _workers)
        delete it;
}

void
workload_generator::run()
{
    configuration *populate_config;
    std::vector<operation_config> operation_configs;
    uint64_t thread_id = 0;

    /* Retrieve useful parameters from the test configuration. */
    operation_configs.push_back(
      operation_config(_config->get_subconfig(INSERT_CONFIG), thread_type::INSERT));
    operation_configs.push_back(
      operation_config(_config->get_subconfig(READ_CONFIG), thread_type::READ));
    operation_configs.push_back(
      operation_config(_config->get_subconfig(UPDATE_CONFIG), thread_type::UPDATE));
    populate_config = _config->get_subconfig(POPULATE_CONFIG);

    /* Populate the database. */
    _database_operation->populate(_database, _timestamp_manager, populate_config, _tracking);
    _db_populated = true;
    delete populate_config;

    /* Generate threads to execute read operations on the collections. */
    for (auto &it : operation_configs) {
        if (it.thread_count != 0)
            logger::log_msg(LOG_INFO,
              "Workload_generator: Creating " + std::to_string(it.thread_count) + " " +
                type_string(it.type) + " threads.");
        for (size_t i = 0; i < it.thread_count && _running; ++i) {
            thread_context *tc = new thread_context(thread_id++, it.type, it.config,
              connection_manager::instance().create_session(), _timestamp_manager, _tracking,
              _database);
            _workers.push_back(tc);
            _thread_manager.add_thread(it.get_func(_database_operation), tc);
        }
        /*
         * Don't forget to delete the config we created earlier. While we do pass the config into
         * the thread context it is not saved, so we are safe to do this.
         */
        delete it.config;

        /*
         * Reset the thread_id counter to 0 as we're only interested in knowing per operation type
         * which thread we are.
         */
        thread_id = 0;
    }
}

void
workload_generator::finish()
{
    component::finish();
    for (const auto &it : _workers)
        it->finish();
    _thread_manager.join();
    logger::log_msg(LOG_TRACE, "Workload generator: run stage done");
}

database &
workload_generator::get_database()
{
    return (_database);
}

bool
workload_generator::db_populated() const
{
    return (_db_populated);
}
} // namespace test_harness
