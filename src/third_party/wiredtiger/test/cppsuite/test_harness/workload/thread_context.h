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

const std::string type_string(thread_type type);

class transaction_context {
    public:
    explicit transaction_context(
      configuration *config, timestamp_manager *timestamp_manager, WT_SESSION *session);

    bool active() const;
    void add_op();
    void begin(const std::string &config = "");
    /* Begin a transaction if we are not currently in one. */
    void try_begin(const std::string &config = "");
    /*
     * Commit a transaction and return true if the commit was successful.
     */
    bool commit(const std::string &config = "");
    /* Rollback a transaction, failure will abort the test. */
    void rollback(const std::string &config = "");
    /* Attempt to rollback the transaction given the requirements are met. */
    void try_rollback(const std::string &config = "");
    /* Set a commit timestamp. */
    void set_commit_timestamp(wt_timestamp_t ts);
    /* Set that the transaction needs to be rolled back. */
    void set_needs_rollback(bool rollback);
    /*
     * Returns true if a transaction can be committed as determined by the op count and the state of
     * the transaction.
     */
    bool can_commit();
    /*
     * Returns true if a transaction can be rolled back as determined by the op count and the state
     * of the transaction.
     */
    bool can_rollback();

    private:
    bool _in_txn = false;
    bool _needs_rollback = false;

    /*
     * _min_op_count and _max_op_count are the minimum and maximum number of operations within one
     * transaction. is the current maximum number of operations that can be executed in the current
     * transaction.
     */
    int64_t _max_op_count = INT64_MAX;
    int64_t _min_op_count = 0;
    /*
     * op_count is the current number of operations that have been executed in the current
     * transaction.
     */
    int64_t _op_count = 0;
    int64_t _target_op_count = 0;

    timestamp_manager *_timestamp_manager = nullptr;
    WT_SESSION *_session = nullptr;
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
     * Return true if the operation was successful, a return value of false implies the transaction
     * needs to be rolled back.
     */
    bool update(scoped_cursor &cursor, uint64_t collection_id, const std::string &key);

    /*
     * Generic insert function, takes a collection_id and key_id, will generate the value. If a
     * timestamp is not specified, the timestamp manager will generate one.
     *
     * Return true if the operation was successful, a return value of false implies the transaction
     * needs to be rolled back.
     */
    bool insert(
      scoped_cursor &cursor, uint64_t collection_id, uint64_t key_id, wt_timestamp_t ts = 0);
    bool insert(
      scoped_cursor &cursor, uint64_t collection_id, const std::string &key, wt_timestamp_t ts = 0);

    /*
     * Generic remove function, takes a collection_id and key and will delete the key if it exists.
     *
     * Return true if the operation was successful, a return value of false implies the transaction
     * needs to be rolled back.
     */
    bool remove(
      scoped_cursor &cursor, uint64_t collection_id, const std::string &key, wt_timestamp_t ts = 0);
    void sleep();
    bool running() const;

    public:
    const int64_t collection_count;
    const int64_t key_count;
    const int64_t key_size;
    const int64_t value_size;
    const int64_t thread_count;
    const thread_type type;
    const uint64_t id;
    database &db;
    scoped_session session;
    scoped_cursor op_track_cursor;
    scoped_cursor stat_cursor;
    timestamp_manager *tsm;
    transaction_context transaction;
    workload_tracking *tracking;

    private:
    bool _running = true;
    throttle _throttle;
};
} // namespace test_harness

#endif
