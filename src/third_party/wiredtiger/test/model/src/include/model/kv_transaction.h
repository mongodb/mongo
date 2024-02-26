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
    prepared,
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
    inline kv_transaction(kv_database &database, txn_id_t id, kv_transaction_snapshot_ptr snapshot,
      timestamp_t read_timestamp = k_timestamp_latest)
        : _database(database), _id(id), _commit_timestamp(k_initial_commit_timestamp),
          _durable_timestamp(k_timestamp_none), _prepare_timestamp(k_timestamp_none),
          _failed(false), _read_timestamp(read_timestamp), _snapshot(snapshot),
          _state(kv_transaction_state::in_progress), _wt_id(k_txn_none),
          _wt_base_write_gen(k_write_gen_none)
    {
        if (!snapshot)
            throw model_exception("The snapshot is NULL.");
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
     * kv_transaction::durable_timestamp --
     *     Get the transaction's durable timestamp, if set.
     */
    inline timestamp_t
    durable_timestamp() const noexcept
    {
        return _durable_timestamp;
    }

    /*
     * kv_transaction::prepare_timestamp --
     *     Get the transaction's prepare timestamp, if set.
     */
    inline timestamp_t
    prepare_timestamp() const noexcept
    {
        return _prepare_timestamp;
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
     * kv_transaction::snapshot --
     *     Get the transaction snapshot.
     */
    inline kv_transaction_snapshot_ptr
    snapshot() const noexcept
    {
        return _snapshot;
    }

    /*
     * kv_transaction::visible_update --
     *     Check whether the given update is visible for this transaction.
     */
    inline bool
    visible_update(const kv_update &update) const noexcept
    {
        return _snapshot->contains(update);
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
    void commit(timestamp_t commit_timestamp = k_timestamp_none,
      timestamp_t durable_timestamp = k_timestamp_none);

    /*
     * kv_transaction::prepare --
     *     Prepare the transaction.
     */
    void prepare(timestamp_t prepare_timestamp);

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
     * kv_transaction::set_commit_timestamp --
     *     Set the commit timestamp for all subsequent updates.
     */
    void set_commit_timestamp(timestamp_t commit_timestamp);

    /*
     * kv_transaction::set_wt_metadata --
     *     If this transaction was imported from WiredTiger, remember the corresponding metadata.
     *     This can be done only before the first update is added to the transaction.
     */
    inline void
    set_wt_metadata(txn_id_t wt_id, write_gen_t wt_base_write_gen)
    {
        std::lock_guard lock_guard(_lock);
        if (!_updates.empty())
            throw model_exception("There are already updates in the transaction");
        _wt_id = wt_id;
        _wt_base_write_gen = wt_base_write_gen;
    }

    /*
     * kv_transaction::wt_id --
     *     Get the WiredTiger ID, if available.
     */
    inline txn_id_t
    wt_id() const
    {
        return _wt_id;
    }

    /*
     * kv_transaction::wt_base_write_gen --
     *     Get the WiredTiger base write generation number, if available.
     */
    inline write_gen_t
    wt_base_write_gen() const
    {
        return _wt_base_write_gen;
    }

protected:
    /*
     * kv_transaction::assert_in_progress_or_prepared --
     *     Assert that the transaction is in progress or prepared.
     */
    void assert_in_progress_or_prepared();

private:
    txn_id_t _id;
    bool _failed;
    std::atomic<kv_transaction_state> _state;

    timestamp_t _commit_timestamp;
    timestamp_t _durable_timestamp;
    timestamp_t _prepare_timestamp;
    timestamp_t _read_timestamp;
    kv_transaction_snapshot_ptr _snapshot;

    /* The lifetime of the transaction must not exceed the lifetime of the database. */
    kv_database &_database;

    mutable std::mutex _lock;
    std::list<std::shared_ptr<kv_transaction_update>> _updates;
    std::list<std::shared_ptr<kv_transaction_update>> _nontimestamped_updates;

    /* Transaction information for updates imported from WiredTiger's debug log. */
    txn_id_t _wt_id;
    write_gen_t _wt_base_write_gen;
};

/*
 * kv_transaction_ptr --
 *     A shared pointer to the transaction.
 */
using kv_transaction_ptr = std::shared_ptr<kv_transaction>;

} /* namespace model */
