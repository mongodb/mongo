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

#include <vector>
#include <string>

extern "C" {
#include "wiredtiger.h"
}

#include "checkpoint_manager.h"
#include "connection_manager.h"
#include "runtime_monitor.h"
#include "workload/database_operation.h"
#include "workload_generator.h"

namespace test_harness {
class test_args {
    public:
    test_args(const std::string &config, const std::string &name, const std::string &wt_open_config)
        : test_config(config), test_name(name), wt_open_config(wt_open_config)
    {
    }
    const std::string test_config;
    const std::string test_name;
    const std::string wt_open_config;
};

/*
 * The base class for a test, the standard usage pattern is to just call run().
 */
class test : public database_operation {
    public:
    test(const test_args &args);
    ~test();

    /* Delete the copy constructor and the assignment operator. */
    test(const test &) = delete;
    test &operator=(const test &) = delete;

    /*
     * The primary run function that most tests will be able to utilize without much other code.
     */
    virtual void run();

    /*
     * Getters for all the major components, used if a test wants more control over the test
     * program.
     */
    workload_generator *get_workload_generator();
    runtime_monitor *get_runtime_monitor();
    timestamp_manager *get_timestamp_manager();
    thread_manager *get_thread_manager();

    protected:
    configuration *_config;

    private:
    const test_args &_args;
    std::vector<component *> _components;
    checkpoint_manager *_checkpoint_manager = nullptr;
    runtime_monitor *_runtime_monitor = nullptr;
    thread_manager *_thread_manager = nullptr;
    timestamp_manager *_timestamp_manager = nullptr;
    workload_generator *_workload_generator = nullptr;
    workload_tracking *_workload_tracking = nullptr;
    /*
     * FIX-ME-Test-Framework: We can't put this code in the destructor of `test` since it will run
     * before the destructors of each of our members (meaning that sessions will get closed after
     * the connection gets closed). To work around this, we've added a member with a destructor that
     * closes the connection.
     */
    struct connection_closer {
        ~connection_closer()
        {
            connection_manager::instance().close();
        }
    } _connection_closer;
    database _database;
};
} // namespace test_harness

#endif
