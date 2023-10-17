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

#ifndef MODEL_KV_TRANSACTION_H
#define MODEL_KV_TRANSACTION_H

#include <atomic>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "model/core.h"
#include "model/data_value.h"
#include "model/kv_transaction_snapshot.h"
#include "model/kv_transaction_update.h"

namespace model {

class kv_database;
class kv_table;
class kv_update;

/*
 * kv_transaction_state --
 *     The transaction state.
 */
enum class kv_transaction_state {
    in_progress,
    committed,
    rolled_back,
};

/*
 * k_initial_commit_timestamp --
 *     The initial commit timestamp that we will temporarily use until the user specifies the actual
 *     commit timestamp.
 */
constexpr timestamp_t k_initial_commit_timestamp = k_timestamp_max;

/*
 * kv_transaction --
 *     A transaction in a key-value database.
 */
class kv_transaction {

public:
    /*
     * kv_transaction::kv_transaction --
     *     Create a new instance of the transaction.
     */
    inline kv_transaction(kv_database &database, txn_id_t id, kv_transaction_snapshot &&snapshot,
      timestamp_t read_timestamp = k_timestamp_latest) noexcept
        : _database(database), _id(id), _commit_timestamp(k_initial_commit_timestamp),
          _failed(false), _read_timestamp(read_timestamp), _snapshot(snapshot),
          _state(kv_transaction_state::in_progress)
    {
    }

    /*
     * kv_transaction::id --
     *     Get the transaction's ID.
     */
    inline txn_id_t
    id() const noexcept
    {
        return _id;
    }

    /*
     * kv_transaction::commit_timestamp --
     *     Get the transaction's commit timestamp, if set.
     */
    inline timestamp_t
    commit_timestamp() const noexcept
    {
        return _commit_timestamp;
    }

    /*
     * kv_transaction::commit_timestamp --
     *     Get the transaction's read timestamp, if set.
     */
    inline timestamp_t
    read_timestamp() const noexcept
    {
        return _read_timestamp;
    }

    /*
     * kv_transaction::failed --
     *     Check whether the transaction has failed.
     */
    inline bool
    failed() const noexcept
    {
        return _failed;
    }

    /*
     * kv_transaction::state --
     *     Get the transaction's state.
     */
    inline kv_transaction_state
    state(std::memory_order order = std::memory_order_acquire) const noexcept
    {
        return _state.load(order);
    }

    /*
     * kv_transaction::visible_txn --
     *     Check whether the given transaction ID is visible for this transaction.
     */
    inline bool
    visible_txn(txn_id_t id) const noexcept
    {
        return _snapshot.contains(id);
    }

    /*
     * kv_transaction::add_update --
     *     Add update. The update should be at this point already incorporated in the table.
     */
    void add_update(kv_table &table, const data_value &key, std::shared_ptr<kv_update> update);

    /*
     * kv_transaction::commit --
     *     Commit the transaction.
     */
    void commit(timestamp_t commit_timestamp = k_timestamp_none);

    /*
     * kv_transaction::fail --
     *     Mark the transaction as failed.
     */
    inline void
    fail() noexcept
    {
        _failed = true;
    }

    /*
     * kv_transaction::reset_snapshot --
     *     Reset the transaction snapshot.
     */
    void reset_snapshot();

    /*
     * kv_transaction::rollback --
     *     Abort the transaction.
     */
    void rollback();

    /*
     * kv_transaction::set_timestamp --
     *     Set the timestamp for all subsequent updates.
     */
    void set_timestamp(timestamp_t commit_timestamp);

private:
    txn_id_t _id;
    bool _failed;
    std::atomic<kv_transaction_state> _state;

    timestamp_t _commit_timestamp;
    timestamp_t _read_timestamp;
    kv_transaction_snapshot _snapshot;

    /* The lifetime of the transaction must not exceed the lifetime of the database. */
    kv_database &_database;

    std::mutex _lock;
    std::list<std::shared_ptr<kv_transaction_update>> _updates;
    std::list<std::shared_ptr<kv_transaction_update>> _nontimestamped_updates;
};

/*
 * kv_transaction_ptr --
 *     A shared pointer to the transaction.
 */
using kv_transaction_ptr = std::shared_ptr<kv_transaction>;

} /* namespace model */
#endif
