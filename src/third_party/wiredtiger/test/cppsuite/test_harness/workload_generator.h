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
#include <functional>

#include "core/component.h"
#include "workload/thread_context.h"
#include "thread_manager.h"
#include "workload/database_operation.h"

/* Forward declarations for classes to reduce compilation time and modules coupling. */
class configuration;
class database;
class workload_tracking;

namespace test_harness {
/*
 * Helper class to enable scalable operation types in the database_operation.
 */
class operation_config {
    public:
    explicit operation_config(configuration *config, thread_type type);

    /* Returns a function pointer to the member function of the supplied database operation. */
    std::function<void(test_harness::thread_context *)> get_func(database_operation *dbo);

    public:
    configuration *config;
    const thread_type type;
    const int64_t thread_count;
};

/*
 * Class that can execute operations based on a given configuration.
 */
class workload_generator : public component {
    public:
    explicit workload_generator(configuration *configuration, database_operation *db_operation,
      timestamp_manager *timestamp_manager, workload_tracking *tracking, database &database);

    ~workload_generator();

    /* Delete the copy constructor and the assignment operator. */
    workload_generator(const workload_generator &) = delete;
    workload_generator &operator=(const workload_generator &) = delete;

    /* Do the work of the main part of the workload. */
    void run() override final;
    void finish() override final;

    database &get_database();
    bool db_populated() const;

    private:
    database &_database;
    database_operation *_database_operation;
    thread_manager _thread_manager;
    timestamp_manager *_timestamp_manager;
    workload_tracking *_tracking;
    std::vector<thread_context *> _workers;
    bool _db_populated = false;
};
} // namespace test_harness

#endif
