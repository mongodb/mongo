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

#include "../core/throttle.h"
#include "../timestamp_manager.h"
#include "database_model.h"
#include "random_generator.h"
#include "workload_tracking.h"

namespace test_harness {
class transaction_context {
    public:
    explicit transaction_context(configuration *config, timestamp_manager *timestamp_manager)
        : _timestamp_manager(timestamp_manager)
    {
        /* Use optional here as our populate threads don't define this configuration. */
        configuration *transaction_config = config->get_optional_subconfig(OPS_PER_TRANSACTION);
        if (transaction_config != nullptr) {
            _min_op_count = transaction_config->get_optional_int(MIN, 1);
            _max_op_count = transaction_config->get_optional_int(MAX, 1);
            delete transaction_config;
        }
    }

    /* Begin a transaction if we are not currently in one. */
    void
    try_begin(WT_SESSION *session, const std::string &config)
    {
        if (!_in_txn)
            begin(session, config);
    }

    void
    begin(WT_SESSION *session, const std::string &config)
    {
        testutil_assert(!_in_txn);
        testutil_check(
          session->begin_transaction(session, config.empty() ? nullptr : config.c_str()));
        /* This randomizes the number of operations to be executed in one transaction. */
        _target_op_count =
          random_generator::instance().generate_integer<int64_t>(_min_op_count, _max_op_count);
        _op_count = 0;
        _in_txn = true;
    }

    bool
    active() const
    {
        return (_in_txn);
    }

    void
    add_op()
    {
        _op_count++;
    }

    /* Attempt to commit the transaction given the requirements are met. */
    void
    try_commit(WT_SESSION *session, const std::string &config)
    {
        if (can_commit_rollback())
            commit(session, config);
    }

    void
    commit(WT_SESSION *session, const std::string &config)
    {
        testutil_assert(_in_txn);
        testutil_check(
          session->commit_transaction(session, config.empty() ? nullptr : config.c_str()));
        _op_count = 0;
        _in_txn = false;
    }

    /* Attempt to rollback the transaction given the requirements are met. */
    void
    try_rollback(WT_SESSION *session, const std::string &config)
    {
        if (can_commit_rollback())
            rollback(session, config);
    }

    void
    rollback(WT_SESSION *session, const std::string &config)
    {
        testutil_assert(_in_txn);
        testutil_check(
          session->rollback_transaction(session, config.empty() ? nullptr : config.c_str()));
        _op_count = 0;
        _in_txn = false;
    }

    /*
     * Set a commit timestamp.
     */
    void
    set_commit_timestamp(WT_SESSION *session, wt_timestamp_t ts)
    {
        /* We don't want to set zero timestamps on transactions if we're not using timestamps. */
        if (!_timestamp_manager->enabled())
            return;
        std::string config = std::string(COMMIT_TS) + "=" + timestamp_manager::decimal_to_hex(ts);
        testutil_check(session->timestamp_transaction(session, config.c_str()));
    }

    private:
    bool
    can_commit_rollback()
    {
        return (_in_txn && _op_count >= _target_op_count);
    }
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

    timestamp_manager *_timestamp_manager = nullptr;
};

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

/* Container class for a thread and any data types it may need to interact with the database. */
class thread_context {
    public:
    thread_context(uint64_t id, thread_type type, configuration *config,
      timestamp_manager *timestamp_manager, workload_tracking *tracking, database &db)
        : id(id), type(type), database(db), timestamp_manager(timestamp_manager),
          tracking(tracking), transaction(transaction_context(config, timestamp_manager)),
          /* These won't exist for certain threads which is why we use optional here. */
          collection_count(config->get_optional_int(COLLECTION_COUNT, 1)),
          key_count(config->get_optional_int(KEY_COUNT_PER_COLLECTION, 1)),
          key_size(config->get_optional_int(KEY_SIZE, 1)),
          value_size(config->get_optional_int(VALUE_SIZE, 1)),
          thread_count(config->get_int(THREAD_COUNT))
    {
        session = connection_manager::instance().create_session();
        _throttle = throttle(config);

        if (tracking->enabled())
            op_track_cursor =
              session.open_scoped_cursor(tracking->get_operation_table_name().c_str());

        testutil_assert(key_size > 0 && value_size > 0);
    }

    virtual ~thread_context() = default;

    void
    finish()
    {
        _running = false;
    }

    /*
     * Convert a key_id to a string. If the resulting string is less than the given length, padding
     * of '0' is added.
     */
    std::string
    key_to_string(uint64_t key_id)
    {
        std::string str, value_str = std::to_string(key_id);
        testutil_assert(key_size >= value_str.size());
        uint64_t diff = key_size - value_str.size();
        std::string s(diff, '0');
        str = s.append(value_str);
        return (str);
    }

    /*
     * Generic update function, takes a collection_id and key, will generate the value.
     *
     * Returns true if it successfully updates the key, false if it receives rollback from the API.
     */
    bool
    update(scoped_cursor &cursor, uint64_t collection_id, const std::string &key)
    {
        WT_DECL_RET;
        std::string value;
        wt_timestamp_t ts = timestamp_manager->get_next_ts();
        testutil_assert(tracking != nullptr);
        testutil_assert(cursor.get() != nullptr);

        transaction.set_commit_timestamp(session.get(), ts);
        value = random_generator::instance().generate_string(value_size);
        cursor->set_key(cursor.get(), key.c_str());
        cursor->set_value(cursor.get(), value.c_str());
        ret = cursor->update(cursor.get());
        if (ret != 0) {
            if (ret == WT_ROLLBACK) {
                transaction.rollback(session.get(), "");
                return (false);
            } else
                testutil_die(ret, "unhandled error while trying to update a key");
        }
        ret = tracking->save_operation(tracking_operation::INSERT, collection_id, key.c_str(),
          value.c_str(), ts, op_track_cursor);
        if (ret != 0) {
            if (ret == WT_ROLLBACK) {
                transaction.rollback(session.get(), "");
                return (false);
            } else
                testutil_die(
                  ret, "unhandled error while trying to save an update to the tracking table");
        }
        transaction.add_op();
        return (true);
    }

    /*
     * Generic insert function, takes a collection_id and key_id, will generate the value.
     *
     * Returns true if it successfully inserts the key, false if it receives rollback from the API.
     */
    bool
    insert(scoped_cursor &cursor, uint64_t collection_id, uint64_t key_id)
    {
        WT_DECL_RET;
        std::string key, value;
        testutil_assert(tracking != nullptr);
        testutil_assert(cursor.get() != nullptr);

        /*
         * Get a timestamp to apply to the update. We still do this even if timestamps aren't
         * enabled as it will return a value for the tracking table.
         */
        wt_timestamp_t ts = timestamp_manager->get_next_ts();
        transaction.set_commit_timestamp(session.get(), ts);

        key = key_to_string(key_id);
        value = random_generator::instance().generate_string(value_size);

        cursor->set_key(cursor.get(), key.c_str());
        cursor->set_value(cursor.get(), value.c_str());
        ret = cursor->insert(cursor.get());
        if (ret != 0) {
            if (ret == WT_ROLLBACK) {
                transaction.rollback(session.get(), "");
                return (false);
            } else
                testutil_die(ret, "unhandled error while trying to insert a key");
        }
        ret = tracking->save_operation(tracking_operation::INSERT, collection_id, key.c_str(),
          value.c_str(), ts, op_track_cursor);
        if (ret != 0) {
            if (ret == WT_ROLLBACK) {
                transaction.rollback(session.get(), "");
                return (false);
            } else
                testutil_die(
                  ret, "unhandled error while trying to save an insert to the tracking table");
        }
        transaction.add_op();
        return (true);
    }

    void
    sleep()
    {
        _throttle.sleep();
    }

    bool
    running() const
    {
        return (_running);
    }

    scoped_session session;
    scoped_cursor op_track_cursor;
    transaction_context transaction;
    test_harness::timestamp_manager *timestamp_manager;
    test_harness::workload_tracking *tracking;
    test_harness::database &database;
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
