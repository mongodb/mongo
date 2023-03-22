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

#include <string>

#include "database_operation.h"
#include "src/component/metrics_monitor.h"
#include "src/component/workload_manager.h"
#include "src/storage/connection_manager.h"

namespace test_harness {
struct test_args {
    const std::string test_config;
    const std::string test_name;
    std::string wt_open_config;
    const std::string home;
};

/*
 * The base class for a test, the standard usage pattern is to just call run().
 */
class test : public database_operation {
public:
    explicit test(const test_args &args);
    virtual ~test();

    /* Delete the copy constructor and the assignment operator. */
    test(const test &) = delete;
    test &operator=(const test &) = delete;

    /* Initialize the operation tracker component and its dependencies. */
    void init_operation_tracker(operation_tracker *op_tracker = nullptr);

    /*
     * The primary run function that most tests will be able to utilize without much other code.
     */
    virtual void run();

protected:
    const test_args &_args;
    configuration *_config;
    timestamp_manager *_timestamp_manager = nullptr;
    operation_tracker *_operation_tracker = nullptr;

private:
    std::vector<component *> _components;
    metrics_monitor *_metrics_monitor = nullptr;
    thread_manager *_thread_manager = nullptr;
    workload_manager *_workload_manager = nullptr;
    database _database;
};
} // namespace test_harness

#endif
