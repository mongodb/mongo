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

#include "database_model.h"
#include "random_generator.h"
#include "workload_tracking.h"

namespace test_harness {
/* Define the different thread operations. */
enum class thread_operation {
    INSERT,
    UPDATE,
    READ,
    REMOVE,
    CHECKPOINT,
    TIMESTAMP,
    MONITOR,
    COMPONENT
};

/* Container class for a thread and any data types it may need to interact with the database. */
class thread_context {
    public:
    thread_context(timestamp_manager *timestamp_manager, workload_tracking *tracking, database &db,
      thread_operation type, int64_t max_op, int64_t min_op, int64_t value_size)
        : _database(db), _current_op_count(0U), _in_txn(false), _running(false), _min_op(min_op),
          _max_op(max_op), _max_op_count(0), _timestamp_manager(timestamp_manager), _type(type),
          _tracking(tracking), _value_size(value_size)
    {
    }

    void
    finish()
    {
        _running = false;
    }

    const std::vector<std::string>
    get_collection_names() const
    {
        return (_database.get_collection_names());
    }

    const keys_iterator_t
    get_collection_keys_begin(const std::string &collection_name) const
    {
        return (_database.get_collection_keys_begin(collection_name));
    }

    const keys_iterator_t
    get_collection_keys_end(const std::string &collection_name) const
    {
        return (_database.get_collection_keys_end(collection_name));
    }

    thread_operation
    get_thread_operation() const
    {
        return (_type);
    }

    workload_tracking *
    get_tracking() const
    {
        return (_tracking);
    }

    int64_t
    get_value_size() const
    {
        return (_value_size);
    }

    bool
    is_running() const
    {
        return (_running);
    }

    bool
    is_in_transaction() const
    {
        return (_in_txn);
    }

    void
    set_running(bool running)
    {
        _running = running;
    }

    void
    begin_transaction(WT_SESSION *session, const std::string &config)
    {
        if (!_in_txn && _timestamp_manager->is_enabled()) {
            testutil_check(
              session->begin_transaction(session, config.empty() ? nullptr : config.c_str()));
            /* This randomizes the number of operations to be executed in one transaction. */
            _max_op_count = random_generator::instance().generate_integer(_min_op, _max_op);
            _current_op_count = 0;
            _in_txn = true;
        }
    }

    /*
     * The current transaction can be committed if:
     *  - The timestamp manager is enabled and
     *  - A transaction has started and
     *      - The thread is done working. This is useful when the test is ended and the thread has
     * not reached the maximum number of operations per transaction or
     *      - The number of operations executed in the current transaction has exceeded the
     * threshold.
     */
    bool
    can_commit_transaction() const
    {
        return (_timestamp_manager->is_enabled() && _in_txn &&
          (!_running || (_current_op_count > _max_op_count)));
    }

    void
    commit_transaction(WT_SESSION *session, const std::string &config)
    {
        /* A transaction cannot be committed if not started. */
        testutil_assert(_in_txn);
        testutil_check(
          session->commit_transaction(session, config.empty() ? nullptr : config.c_str()));
        _in_txn = false;
    }

    void
    increment_operation_count(uint64_t inc = 1)
    {
        _current_op_count += inc;
    }

    /*
     * Set a commit timestamp if the timestamp manager is enabled and always return the timestamp
     * that should have been used for the commit.
     */
    wt_timestamp_t
    set_commit_timestamp(WT_SESSION *session)
    {

        wt_timestamp_t ts = _timestamp_manager->get_next_ts();
        std::string config;

        if (_timestamp_manager->is_enabled()) {
            config = std::string(COMMIT_TS) + "=" + _timestamp_manager->decimal_to_hex(ts);
            testutil_check(session->timestamp_transaction(session, config.c_str()));
        }

        return (ts);
    }

    private:
    /* Representation of the collections and their key/value pairs in memory. */
    database _database;
    /*
     * _current_op_count is the current number of operations that have been executed in the current
     * transaction.
     */
    uint64_t _current_op_count;
    bool _in_txn, _running;
    /*
     * _min_op and _max_op are the minimum and maximum number of operations within one transaction.
     * _max_op_count is the current maximum number of operations that can be executed in the current
     * transaction. _max_op_count will always be <= _max_op.
     */
    int64_t _min_op, _max_op, _max_op_count;
    timestamp_manager *_timestamp_manager;
    const thread_operation _type;
    workload_tracking *_tracking;
    /* Temporary member that comes from the test configuration. */
    int64_t _value_size;
};
} // namespace test_harness

#endif
