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

#ifndef THREAD_CONTEXT_H
#define THREAD_CONTEXT_H

#include <string>

extern "C" {
#include "test_util.h"
}

#include "../core/throttle.h"
#include "../workload/database_model.h"

/* Forward declarations for classes to reduce compilation time and modules coupling. */
class configuration;
class timestamp_manager;
class workload_tracking;

namespace test_harness {
enum thread_type { READ, INSERT, UPDATE };

static std::string
type_string(thread_type type)
{
    switch (type) {
    case thread_type::INSERT:
        return ("insert");
    case thread_type::READ:
        return ("read");
    case thread_type::UPDATE:
        return ("update");
    default:
        testutil_die(EINVAL, "unexpected thread_type: %d", static_cast<int>(type));
    }
}

class transaction_context {
    public:
    explicit transaction_context(
      configuration *config, timestamp_manager *timestamp_manager, WT_SESSION *session);

    bool active() const;
    void add_op();
    void begin(const std::string &config = "");
    /* Begin a transaction if we are not currently in one. */
    void try_begin(const std::string &config = "");
    void commit(const std::string &config = "");
    /* Attempt to commit the transaction given the requirements are met. */
    void try_commit(const std::string &config = "");
    void rollback(const std::string &config = "");
    /* Attempt to rollback the transaction given the requirements are met. */
    void try_rollback(const std::string &config = "");
    /* Set a commit timestamp. */
    void set_commit_timestamp(wt_timestamp_t ts);

    private:
    bool can_commit_rollback();

    private:
    /*
     * op_count is the current number of operations that have been executed in the current
     * transaction.
     */
    int64_t _op_count = 0;

    /*
     * _min_op_count and _max_op_count are the minimum and maximum number of operations within one
     * transaction. is the current maximum number of operations that can be executed in the current
     * transaction.
     */
    int64_t _min_op_count = 0;
    int64_t _max_op_count = INT64_MAX;
    int64_t _target_op_count = 0;
    bool _in_txn = false;

    WT_SESSION *_session = nullptr;
    timestamp_manager *_timestamp_manager = nullptr;
};

/* Container class for a thread and any data types it may need to interact with the database. */
class thread_context {
    public:
    thread_context(uint64_t id, thread_type type, configuration *config,
      scoped_session &&created_session, timestamp_manager *timestamp_manager,
      workload_tracking *tracking, database &dbase);

    virtual ~thread_context() = default;

    void finish();

    /*
     * Convert a key_id to a string. If the resulting string is less than the given length, padding
     * of '0' is added.
     */
    std::string key_to_string(uint64_t key_id);

    /*
     * Generic update function, takes a collection_id and key, will generate the value.
     *
     * Returns true if it successfully updates the key, false if it receives rollback from the API.
     */
    bool update(scoped_cursor &cursor, uint64_t collection_id, const std::string &key);

    /*
     * Generic insert function, takes a collection_id and key_id, will generate the value.
     *
     * Returns true if it successfully inserts the key, false if it receives rollback from the API.
     */
    bool insert(scoped_cursor &cursor, uint64_t collection_id, uint64_t key_id);

    /*
     * Generic next function.
     *
     * Handles rollback and not found internally, but will return the error code to the caller so
     * the caller can distinguish between them.
     */
    int next(scoped_cursor &cursor);

    void sleep();
    bool running() const;

    public:
    scoped_session session;
    scoped_cursor op_track_cursor;
    transaction_context transaction;
    timestamp_manager *tsm;
    workload_tracking *tracking;
    database &db;
    const int64_t collection_count;
    const int64_t key_count;
    const int64_t key_size;
    const int64_t value_size;
    const int64_t thread_count;
    const uint64_t id;
    const thread_type type;

    private:
    throttle _throttle;
    bool _running = true;
};
} // namespace test_harness

#endif
