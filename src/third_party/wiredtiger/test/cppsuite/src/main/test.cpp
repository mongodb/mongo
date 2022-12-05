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

#include "test.h"

#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/component/metrics_writer.h"

namespace test_harness {
test::test(const test_args &args) : _args(args)
{
    _config = new configuration(args.test_name, args.test_config);
    _metrics_monitor =
      new metrics_monitor(args.test_name, _config->get_subconfig(METRICS_MONITOR), _database);
    _timestamp_manager = new timestamp_manager(_config->get_subconfig(TIMESTAMP_MANAGER));
    _workload_manager = new workload_manager(
      _config->get_subconfig(WORKLOAD_MANAGER), this, _timestamp_manager, _database);
    _thread_manager = new thread_manager();

    _database.set_timestamp_manager(_timestamp_manager);
    _database.set_create_config(
      _config->get_bool(COMPRESSION_ENABLED), _config->get_bool(REVERSE_COLLATOR));

    /*
     * Ordering is not important here, any dependencies between components should be resolved
     * internally by the components.
     */
    _components = {_workload_manager, _timestamp_manager, _metrics_monitor};
}

void
test::init_operation_tracker(operation_tracker *op_tracker)
{
    delete _operation_tracker;
    if (op_tracker == nullptr) {
        /* Fallback to default behavior. */
        op_tracker = new operation_tracker(_config->get_subconfig(OPERATION_TRACKER),
          _config->get_bool(COMPRESSION_ENABLED), *_timestamp_manager);
    }
    _operation_tracker = op_tracker;
    _workload_manager->set_operation_tracker(_operation_tracker);
    _database.set_operation_tracker(_operation_tracker);
    _components.push_back(_operation_tracker);
}

test::~test()
{
    delete _config;
    delete _metrics_monitor;
    delete _timestamp_manager;
    delete _thread_manager;
    delete _workload_manager;
    delete _operation_tracker;
    _config = nullptr;
    _metrics_monitor = nullptr;
    _timestamp_manager = nullptr;
    _thread_manager = nullptr;
    _workload_manager = nullptr;
    _operation_tracker = nullptr;

    _components.clear();
}

void
test::run()
{
    int64_t cache_max_wait_ms, cache_size_mb, duration_seconds;
    bool enable_logging, statistics_logging;
    configuration *statistics_config;
    std::string statistics_type;
    /* Build the database creation config string. Allow for a maximum 1024 sessions. */
    std::string db_create_config = CONNECTION_CREATE + ",session_max=1024";
    /* Enable snappy compression or reverse collator if required. */
    if (_config->get_bool(COMPRESSION_ENABLED) || _config->get_bool(REVERSE_COLLATOR)) {
        db_create_config += ",extensions=[";
        db_create_config +=
          _config->get_bool(COMPRESSION_ENABLED) ? std::string(SNAPPY_PATH) + "," : "";
        db_create_config +=
          _config->get_bool(REVERSE_COLLATOR) ? std::string(REVERSE_COLLATOR_PATH) : "";
        db_create_config += "]";
    }

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

    /* Maximum waiting time for the cache to get unstuck. */
    cache_max_wait_ms = _config->get_int(CACHE_MAX_WAIT_MS);
    db_create_config += ",cache_max_wait_ms=" + std::to_string(cache_max_wait_ms);

    /* Add the user supplied wiredtiger open config. */
    db_create_config += "," + _args.wt_open_config;

    /* Create connection. */
    connection_manager::instance().create(
      db_create_config, _args.home.empty() ? DEFAULT_DIR : _args.home);

    /* Initiate the load stage of each component. */
    for (const auto &it : _components)
        it->load();

    /* Spawn threads for all component::run() functions. */
    for (const auto &it : _components)
        _thread_manager->add_thread(&component::run, it);

    /* The initial population phase needs to be finished before starting the actual test. */
    while (_workload_manager->enabled() && !_workload_manager->db_populated())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    /* The test will run for the duration as defined in the config. */
    duration_seconds = _config->get_int(DURATION_SECONDS);
    testutil_assert(duration_seconds >= 0);
    logger::log_msg(LOG_INFO,
      "Waiting {" + std::to_string(duration_seconds) + "} seconds for testing to complete.");
    std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));

    /* Notify components that they should complete their last iteration. */
    for (const auto &it : _components)
        it->end_run();

    /* Call join on the components threads so we know they have finished their loop. */
    logger::log_msg(LOG_INFO,
      "Joining all component threads.\n This could take a while as we need to wait"
      " for all components to finish their current loop.");
    _thread_manager->join();

    /* End the test by calling finish on all known components. */
    for (const auto &it : _components)
        it->finish();

    /* Validation stage. */
    if (_operation_tracker->enabled()) {
        std::unique_ptr<configuration> tracking_config(_config->get_subconfig(OPERATION_TRACKER));
        this->validate(_operation_tracker->get_operation_table_name(),
          _operation_tracker->get_schema_table_name(), _workload_manager->get_database());
    }

    /* Log perf stats. */
    metrics_writer::instance().output_perf_file(_args.test_name);

    logger::log_msg(LOG_INFO, "SUCCESS");
}
} // namespace test_harness
