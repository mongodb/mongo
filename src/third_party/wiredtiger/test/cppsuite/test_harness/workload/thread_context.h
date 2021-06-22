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
    explicit transaction_context(configuration *config)
    {
        configuration *transaction_config = config->get_subconfig(OPS_PER_TRANSACTION);
        _min_op_count = transaction_config->get_int(MIN);
        _max_op_count = transaction_config->get_int(MAX);
        delete transaction_config;
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
        op_count = 0;
        _in_txn = true;
    }

    bool
    active() const
    {
        return (_in_txn);
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
        op_count = 0;
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
        op_count = 0;
        _in_txn = false;
    }

    /*
     * Set a commit timestamp.
     */
    void
    set_commit_timestamp(WT_SESSION *session, const std::string &ts)
    {
        std::string config = std::string(COMMIT_TS) + "=" + ts;
        testutil_check(session->timestamp_transaction(session, config.c_str()));
    }

    /*
     * op_count is the current number of operations that have been executed in the current
     * transaction.
     */
    int64_t op_count = 0;

    private:
    bool
    can_commit_rollback()
    {
        return (_in_txn && op_count >= _target_op_count);
    }

    /*
     * _min_op_count and _max_op_count are the minimum and maximum number of operations within one
     * transaction. is the current maximum number of operations that can be executed in the current
     * transaction.
     */
    int64_t _min_op_count = 0;
    int64_t _max_op_count = INT64_MAX;
    int64_t _target_op_count = 0;
    bool _in_txn = false;
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
          tracking(tracking), transaction(transaction_context(config)),
          /* These won't exist for read threads which is why we use optional here. */
          key_size(config->get_optional_int(KEY_SIZE, 1)),
          value_size(config->get_optional_int(VALUE_SIZE, 1)),
          thread_count(config->get_int(THREAD_COUNT))
    {
        session = connection_manager::instance().create_session();
        _throttle = throttle(config);

        if (tracking->enabled())
            testutil_check(session->open_cursor(session,
              tracking->get_operation_table_name().c_str(), nullptr, nullptr, &op_track_cursor));

        testutil_assert(key_size > 0 && value_size > 0);
    }

    virtual ~thread_context()
    {
        if (op_track_cursor != nullptr)
            testutil_check(op_track_cursor->close(op_track_cursor));
    }

    void
    finish()
    {
        _running = false;
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

    WT_SESSION *session;
    WT_CURSOR *op_track_cursor = nullptr;
    transaction_context transaction;
    test_harness::timestamp_manager *timestamp_manager;
    test_harness::workload_tracking *tracking;
    test_harness::database &database;
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
