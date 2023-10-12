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
    _timestamp_manager = new timestamp_manager(_config->get_subconfig(TIMESTAMP_MANAGER));
    _workload_manager = new workload_manager(
      _config->get_subconfig(WORKLOAD_MANAGER), this, _timestamp_manager, _database);
    _thread_manager = new thread_manager();

    /* Only create the metrics monitor if enabled. */
    auto metrics_monitor_cfg = _config->get_subconfig(METRICS_MONITOR);
    if (metrics_monitor_cfg->get_bool(ENABLED))
        _metrics_monitor = new metrics_monitor(args.test_name, metrics_monitor_cfg, _database);
    else
        delete metrics_monitor_cfg;

    _database.set_timestamp_manager(_timestamp_manager);
    _database.set_create_config(
      _config->get_bool(COMPRESSION_ENABLED), _config->get_bool(REVERSE_COLLATOR));

    /* Update the component list with the enabled ones. */
    _components.push_back(_workload_manager);
    if (_timestamp_manager->enabled())
        _components.push_back(_timestamp_manager);
    if (_metrics_monitor != nullptr && _metrics_monitor->enabled())
        _components.push_back(_metrics_monitor);
}

void
test::init_operation_tracker(operation_tracker *op_tracker)
{
    delete _operation_tracker;
    std::unique_ptr<configuration> operation_tracker_cfg(_config->get_subconfig(OPERATION_TRACKER));
    bool tracking_enabled = operation_tracker_cfg->get_bool(ENABLED);
    if (op_tracker == nullptr) {
        /* Fallback to default behavior. */
        op_tracker = new operation_tracker(_config->get_subconfig(OPERATION_TRACKER),
          _config->get_bool(COMPRESSION_ENABLED), *_timestamp_manager);
    } else {
        /*
         * If a custom tracker has been given, make sure it has been enabled in the test
         * configuration.
         */
        testutil_assert(tracking_enabled);
    }
    _operation_tracker = op_tracker;
    _workload_manager->set_operation_tracker(_operation_tracker);
    _database.set_operation_tracker(_operation_tracker);

    if (tracking_enabled)
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
    int64_t cache_size_mb;
    std::chrono::milliseconds cache_max_wait_ms;
    std::chrono::seconds duration_seconds;
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

    /* Enable or disable background compact debug mode. */
    if (_config->get_bool(BACKGROUND_COMPACT_DEBUG_MODE))
        db_create_config += ",debug_mode=(background_compact)";

    /* Maximum waiting time for the cache to get unstuck. */
    cache_max_wait_ms = std::chrono::milliseconds(_config->get_int(CACHE_MAX_WAIT_MS));
    db_create_config += ",cache_max_wait_ms=" + std::to_string(cache_max_wait_ms.count());

    /* Add the user supplied wiredtiger open config. */
    db_create_config += "," + _args.wt_open_config;

    /* Create connection. */
    connection_manager::instance().create(
      db_create_config, _args.home.empty() ? DEFAULT_DIR : _args.home);

    /* Load each component. They have to be all loaded first before being able to run. */
    for (const auto &it : _components)
        it->load();

    /* Run each component. */
    for (const auto &it : _components)
        _thread_manager->add_thread(&component::run, it);

    /* The initial population phase needs to be finished before starting the actual test. */
    while (!_workload_manager->db_populated())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    /* The test will run for the duration as defined in the config. */
    duration_seconds = std::chrono::seconds(_config->get_int(DURATION_SECONDS));
    testutil_assert(duration_seconds.count() >= 0);
    logger::log_msg(LOG_INFO,
      "Waiting {" + std::to_string(duration_seconds.count()) +
        "} seconds for testing to complete.");
    std::this_thread::sleep_for(duration_seconds);

    /* Notify components that they should stop. */
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
    if (_config->get_bool(VALIDATE))
        this->validate(_operation_tracker->enabled(),
          _operation_tracker->get_operation_table_name(),
          _operation_tracker->get_schema_table_name(), _workload_manager->get_database());

    /* Log perf stats. */
    metrics_writer::instance().output_perf_file(_args.test_name);

    logger::log_msg(LOG_INFO, "SUCCESS");
}
} // namespace test_harness
