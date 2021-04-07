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

#ifndef TEST_H
#define TEST_H

/* Required to build using older versions of g++. */
#include <cinttypes>
#include <vector>
#include <mutex>

extern "C" {
#include "wiredtiger.h"
}

#include "api_const.h"
#include "component.h"
#include "configuration.h"
#include "connection_manager.h"
#include "runtime_monitor.h"
#include "timestamp_manager.h"
#include "thread_manager.h"
#include "workload_generator.h"
#include "workload_validation.h"

namespace test_harness {
/*
 * The base class for a test, the standard usage pattern is to just call run().
 */
class test : public database_operation {
    public:
    test(const std::string &config, const std::string &name)
        : _runtime_monitor(nullptr), _thread_manager(nullptr), _timestamp_manager(nullptr),
          _workload_generator(nullptr), _workload_tracking(nullptr)
    {
        _configuration = new configuration(name, config);
        _runtime_monitor = new runtime_monitor(_configuration->get_subconfig(RUNTIME_MONITOR));
        _timestamp_manager =
          new timestamp_manager(_configuration->get_subconfig(TIMESTAMP_MANAGER));
        _workload_tracking = new workload_tracking(_configuration->get_subconfig(WORKLOAD_TRACKING),
          OPERATION_TRACKING_TABLE_CONFIG, TABLE_OPERATION_TRACKING, SCHEMA_TRACKING_TABLE_CONFIG,
          TABLE_SCHEMA_TRACKING);
        _workload_generator =
          new workload_generator(_configuration->get_subconfig(WORKLOAD_GENERATOR), this,
            _timestamp_manager, _workload_tracking);
        _thread_manager = new thread_manager();
        /*
         * Ordering is not important here, any dependencies between components should be resolved
         * internally by the components.
         */
        _components = {
          _workload_tracking, _workload_generator, _timestamp_manager, _runtime_monitor};
    }

    ~test()
    {
        delete _configuration;
        delete _runtime_monitor;
        delete _timestamp_manager;
        delete _thread_manager;
        delete _workload_generator;
        delete _workload_tracking;
        _configuration = nullptr;
        _runtime_monitor = nullptr;
        _timestamp_manager = nullptr;
        _thread_manager = nullptr;
        _workload_generator = nullptr;
        _workload_tracking = nullptr;

        _components.clear();
    }

    /* Delete the copy constructor and the assignment operator. */
    test(const test &) = delete;
    test &operator=(const test &) = delete;

    /*
     * The primary run function that most tests will be able to utilize without much other code.
     */
    virtual void
    run()
    {
        int64_t cache_size_mb = 100, duration_seconds = 0;
        bool enable_logging, is_success = true;

        /* Build the database creation config string. */
        std::string db_create_config = CONNECTION_CREATE;

        testutil_check(_configuration->get_int(CACHE_SIZE_MB, cache_size_mb));
        db_create_config += ",statistics=(fast),cache_size=" + std::to_string(cache_size_mb) + "MB";
        testutil_check(_configuration->get_bool(ENABLE_LOGGING, enable_logging));
        db_create_config += ",log=(enabled=" + std::string(enable_logging ? "true" : "false") + ")";

        /* Set up the test environment. */
        connection_manager::instance().create(db_create_config);

        /* Initiate the load stage of each component. */
        for (const auto &it : _components)
            it->load();

        /* Spawn threads for all component::run() functions. */
        for (const auto &it : _components)
            _thread_manager->add_thread(&component::run, it);

        /* Sleep duration seconds. */
        testutil_check(_configuration->get_int(DURATION_SECONDS, duration_seconds));
        testutil_assert(duration_seconds >= 0);
        std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));

        /* End the test. */
        for (const auto &it : _components)
            it->finish();
        _thread_manager->join();

        /* Validation stage. */
        if (_workload_tracking->is_enabled()) {
            workload_validation wv;
            is_success = wv.validate(_workload_tracking->get_operation_table_name(),
              _workload_tracking->get_schema_table_name(), _workload_generator->get_database());
        }

        debug_print(is_success ? "SUCCESS" : "FAILED", DEBUG_INFO);
        connection_manager::instance().close();
    }

    /*
     * Getters for all the major components, used if a test wants more control over the test
     * program.
     */
    workload_generator *
    get_workload_generator()
    {
        return _workload_generator;
    }

    runtime_monitor *
    get_runtime_monitor()
    {
        return _runtime_monitor;
    }

    timestamp_manager *
    get_timestamp_manager()
    {
        return _timestamp_manager;
    }

    thread_manager *
    get_thread_manager()
    {
        return _thread_manager;
    }

    private:
    std::string _name;
    std::vector<component *> _components;
    configuration *_configuration;
    runtime_monitor *_runtime_monitor;
    thread_manager *_thread_manager;
    timestamp_manager *_timestamp_manager;
    workload_generator *_workload_generator;
    workload_tracking *_workload_tracking;
};
} // namespace test_harness

#endif
