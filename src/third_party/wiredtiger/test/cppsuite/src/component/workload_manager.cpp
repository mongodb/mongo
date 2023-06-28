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
#include <memory>

#include "workload_manager.h"

#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/main/operation_configuration.h"
#include "src/storage/connection_manager.h"
#include "src/util/barrier.h"

namespace test_harness {
workload_manager::workload_manager(configuration *configuration, database_operation *db_operation,
  timestamp_manager *timestamp_manager, database &database)
    : component(WORKLOAD_MANAGER, configuration), _database(database),
      _database_operation(db_operation), _timestamp_manager(timestamp_manager)
{
}

workload_manager::~workload_manager()
{
    for (auto &it : _workers)
        delete it;
}

void
workload_manager::set_operation_tracker(operation_tracker *op_tracker)
{
    testutil_assert(_operation_tracker == nullptr);
    _operation_tracker = op_tracker;
}

void
workload_manager::run()
{
    configuration *populate_config;
    std::vector<operation_configuration> operation_configs;
    uint64_t thread_id = 0;

    /* Retrieve useful parameters from the test configuration. */
    operation_configs.push_back(operation_configuration(
      _config->get_subconfig(CHECKPOINT_OP_CONFIG), thread_type::CHECKPOINT));
    operation_configs.push_back(
      operation_configuration(_config->get_subconfig(CUSTOM_OP_CONFIG), thread_type::CUSTOM));
    operation_configs.push_back(
      operation_configuration(_config->get_subconfig(INSERT_OP_CONFIG), thread_type::INSERT));
    operation_configs.push_back(
      operation_configuration(_config->get_subconfig(READ_OP_CONFIG), thread_type::READ));
    operation_configs.push_back(
      operation_configuration(_config->get_subconfig(REMOVE_OP_CONFIG), thread_type::REMOVE));
    operation_configs.push_back(
      operation_configuration(_config->get_subconfig(UPDATE_OP_CONFIG), thread_type::UPDATE));
    populate_config = _config->get_subconfig(POPULATE_CONFIG);

    /* Populate the database. */
    _database_operation->populate(
      _database, _timestamp_manager, populate_config, _operation_tracker);
    _db_populated = true;
    delete populate_config;

    /* Generate threads to execute the different operations on the collections. */
    for (auto &it : operation_configs) {
        if (it.thread_count != 0)
            logger::log_msg(LOG_INFO,
              "workload_manager: Creating " + std::to_string(it.thread_count) + " " +
                type_string(it.type) + " threads.");
        /* Create a synchronization object to provide to the thread workers. */
        std::shared_ptr<barrier> barrier_ptr = std::make_shared<barrier>(it.thread_count);
        for (size_t i = 0; i < it.thread_count && _running; ++i) {
            thread_worker *tc = new thread_worker(thread_id++, it.type, it.config,
              connection_manager::instance().create_session(), _timestamp_manager,
              _operation_tracker, _database, barrier_ptr);
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
workload_manager::finish()
{
    component::finish();
    for (const auto &it : _workers)
        it->finish();
    _thread_manager.join();
    logger::log_msg(LOG_TRACE, "Workload generator: run stage done");
}

database &
workload_manager::get_database()
{
    return (_database);
}

bool
workload_manager::db_populated() const
{
    return (_db_populated);
}
} // namespace test_harness
