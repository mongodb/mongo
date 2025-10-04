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

#pragma once

#include "database.h"
#include "thread_worker.h"

namespace test_harness {
class database_operation {
public:
    /*
     * Function that performs the following steps using the configuration that is defined by the
     * test:
     *  - Creates N collections as per the configuration.
     *  - Creates M threads as per the configuration, each thread will:
     *      - Open a cursor on each collection.
     *      - Insert K key/value pairs in each collection. Values are random strings which size is
     * defined by the configuration.
     */
    virtual void populate(database &database, timestamp_manager *tsm, configuration *config,
      operation_tracker *op_tracker);

    /* Enabled the background compaction server. */
    virtual void background_compact_operation(thread_worker *tc);

    /* Performs a checkpoint periodically. */
    virtual void checkpoint_operation(thread_worker *tc);

    /* Custom operation without a default implementation. */
    virtual void custom_operation(thread_worker *tc);

    /* Basic insert operation that adds a new key every rate tick. */
    virtual void insert_operation(thread_worker *tc);

    /* Basic read operation that chooses a random collection and walks a cursor. */
    virtual void read_operation(thread_worker *tc);

    /* Basic remove operation that chooses a random key and deletes it. */
    virtual void remove_operation(thread_worker *tc);

    /* Basic update operation that chooses a random key and updates it. */
    virtual void update_operation(thread_worker *tc);

    virtual void validate(bool tracking_enabled, const std::string &operation_table_name,
      const std::string &schema_table_name, database &db);

    virtual ~database_operation() = default;
};
} // namespace test_harness
