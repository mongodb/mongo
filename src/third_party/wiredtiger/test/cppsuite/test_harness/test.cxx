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

/* Required to build using older versions of g++. */
#include <cinttypes>
#include <memory>
#include <mutex>

#include "connection_manager.h"
#include "core/component.h"
#include "core/configuration.h"
#include "test.h"
#include "thread_manager.h"
#include "timestamp_manager.h"
#include "workload/workload_validation.h"
#include "util/api_const.h"

namespace test_harness {
test::test(const test_args &args) : _args(args)
{
    _config = new configuration(args.test_name, args.test_config);
    _checkpoint_manager = new checkpoint_manager(_config->get_subconfig(CHECKPOINT_MANAGER));
    _runtime_monitor =
      new runtime_monitor(args.test_name, _config->get_subconfig(RUNTIME_MONITOR), _database);
    _timestamp_manager = new timestamp_manager(_config->get_subconfig(TIMESTAMP_MANAGER));
    _workload_tracking = new workload_tracking(_config->get_subconfig(WORKLOAD_TRACKING),
      OPERATION_TRACKING_TABLE_CONFIG, TABLE_OPERATION_TRACKING, SCHEMA_TRACKING_TABLE_CONFIG,
      TABLE_SCHEMA_TRACKING, _config->get_bool(COMPRESSION_ENABLED), *_timestamp_manager);
    _workload_generator = new workload_generator(_config->get_subconfig(WORKLOAD_GENERATOR), this,
      _timestamp_manager, _workload_tracking, _database);
    _thread_manager = new thread_manager();

    _database.set_timestamp_manager(_timestamp_manager);
    _database.set_workload_tracking(_workload_tracking);
    _database.set_create_config(_config->get_bool(COMPRESSION_ENABLED));

    /*
     * Ordering is not important here, any dependencies between components should be resolved
     * internally by the components.
     */
    _components = {_workload_tracking, _workload_generator, _timestamp_manager, _runtime_monitor,
      _checkpoint_manager};
}

test::~test()
{
    delete _config;
    delete _checkpoint_manager;
    delete _runtime_monitor;
    delete _timestamp_manager;
    delete _thread_manager;
    delete _workload_generator;
    delete _workload_tracking;
    _config = nullptr;
    _checkpoint_manager = nullptr;
    _runtime_monitor = nullptr;
    _timestamp_manager = nullptr;
    _thread_manager = nullptr;
    _workload_generator = nullptr;
    _workload_tracking = nullptr;

    _components.clear();
}

void
test::run()
{
    int64_t cache_size_mb, duration_seconds;
    bool enable_logging, statistics_logging;
    configuration *statistics_config;
    std::string statistics_type;
    /* Build the database creation config string. */
    std::string db_create_config = CONNECTION_CREATE;

    /* Enable snappy compression if required. */
    if (_config->get_bool(COMPRESSION_ENABLED))
        db_create_config += SNAPPY_EXT;

    /* Get the cache size. */
    cache_size_mb = _config->get_int(CACHE_SIZE_MB);
    db_create_config += ",cache_size=" + std::to_string(cache_size_mb) + "MB";

    /* Get the statistics configuration for this run. */
    statistics_config = _config->get_subconfig(STATISTICS_CONFIG);
    statistics_type = statistics_config->get_string(TYPE);
    statistics_logging = statistics_config->get_bool(ENABLE_LOGGING);
    db_create_config += statistics_logging ? "," + STATISTICS_LOG : "";
    db_create_config += ",statistics=(" + statistics_type + ")";
    /* Don't forget to delete. */
    delete statistics_config;

    /* Enable or disable write ahead logging. */
    enable_logging = _config->get_bool(ENABLE_LOGGING);
    db_create_config += ",log=(enabled=" + std::string(enable_logging ? "true" : "false") + ")";

    /* Add the user supplied wiredtiger open config. */
    db_create_config += _args.wt_open_config;

    /*
     * Set up the test environment. A smart pointer is used here so that the connection can
     * automatically be closed by the scoped_connection's destructor when the test finishes and the
     * pointer goes out of scope.
     */
    _scoped_conn = std::make_shared<scoped_connection>(db_create_config);

    /* Initiate the load stage of each component. */
    for (const auto &it : _components)
        it->load();

    /* Spawn threads for all component::run() functions. */
    for (const auto &it : _components)
        _thread_manager->add_thread(&component::run, it);

    /* The initial population phase needs to be finished before starting the actual test. */
    while (_workload_generator->enabled() && !_workload_generator->db_populated())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    /* The test will run for the duration as defined in the config. */
    duration_seconds = _config->get_int(DURATION_SECONDS);
    testutil_assert(duration_seconds >= 0);
    logger::log_msg(LOG_INFO,
      "Waiting {" + std::to_string(duration_seconds) + "} seconds for testing to complete.");
    std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));

    /* End the test by calling finish on all known components. */
    for (const auto &it : _components)
        it->finish();

    logger::log_msg(LOG_INFO,
      "Joining all component threads.\n This could take a while as we need to wait"
      " for all components to finish their current loop.");
    _thread_manager->join();

    /* Validation stage. */
    if (_workload_tracking->enabled()) {
        workload_validation wv;
        wv.validate(_workload_tracking->get_operation_table_name(),
          _workload_tracking->get_schema_table_name(),
          _workload_generator->get_database().get_collection_ids());
    }

    logger::log_msg(LOG_INFO, "SUCCESS");
}
} // namespace test_harness
