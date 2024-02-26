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

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "model/kv_checkpoint.h"
#include "model/kv_table.h"
#include "model/kv_transaction.h"

namespace model {

/*
 * kv_database --
 *     A database with key-value tables.
 */
class kv_database {

public:
    /*
     * kv_database::kv_database --
     *     Create a new instance.
     */
    inline kv_database() : _last_transaction_id(k_txn_none), _stable_timestamp(k_timestamp_none) {}

    /*
     * kv_database::create_checkpoint --
     *     Create a checkpoint. Throw an exception if the name is not unique.
     */
    kv_checkpoint_ptr create_checkpoint(const char *name = nullptr);

    /*
     * kv_database::create_checkpoint --
     *     Create a checkpoint from custom metadata. Throw an exception if the name is not unique.
     */
    kv_checkpoint_ptr create_checkpoint(
      const char *name, kv_transaction_snapshot_ptr snapshot, timestamp_t stable_timestamp);

    /*
     * kv_database::create_table --
     *     Create and return a new table. Throw an exception if the name is not unique.
     */
    inline kv_table_ptr
    create_table(const std::string &name, const kv_table_config &config = kv_table_config{})
    {
        return create_table(name.c_str(), config);
    }

    /*
     * kv_database::create_table --
     *     Create and return a new table. Throw an exception if the name is not unique.
     */
    kv_table_ptr create_table(const char *name, const kv_table_config &config = kv_table_config{});

    /*
     * kv_database::checkpoint --
     *     Get the checkpoint. Throw an exception if it does not exist.
     */
    inline kv_checkpoint_ptr
    checkpoint(const std::string &name) const
    {
        std::lock_guard lock_guard(_checkpoints_lock);
        auto i = _checkpoints.find(name);
        if (i == _checkpoints.end())
            throw model_exception("No such checkpoint: " + name);
        return i->second;
    }

    /*
     * kv_database::checkpoint --
     *     Get the checkpoint. Throw an exception if it does not exist.
     */
    kv_checkpoint_ptr checkpoint(const char *name = nullptr);

    /*
     * kv_transaction::set_stable_timestamp --
     *     Set the database's stable timestamp, if set.
     */
    inline void
    set_stable_timestamp(timestamp_t timestamp) noexcept
    {
        std::lock_guard lock_guard(_timestamps_lock);
        _stable_timestamp = std::max(_stable_timestamp, timestamp);
    }

    /*
     * kv_transaction::stable_timestamp --
     *     Get the database's stable timestamp, if set.
     */
    inline timestamp_t
    stable_timestamp() const noexcept
    {
        return _stable_timestamp;
    }

    /*
     * kv_database::contains_table --
     *     Check whether the database contains the given table.
     */
    inline bool
    contains_table(const std::string &name) const
    {
        std::lock_guard lock_guard(_tables_lock);
        auto i = _tables.find(name);
        return i != _tables.end();
    }

    /*
     * kv_database::table --
     *     Get the table. Throw an exception if it does not exist.
     */
    inline kv_table_ptr
    table(const std::string &name)
    {
        std::lock_guard lock_guard(_tables_lock);
        auto i = _tables.find(name);
        if (i == _tables.end())
            throw model_exception("No such table: " + name);
        return i->second;
    }

    /*
     * kv_database::table --
     *     Get the table. Throw an exception if it does not exist.
     */
    inline kv_table_ptr
    table(const char *name)
    {
        std::string table_name = name;
        return table(table_name);
    }

    /*
     * kv_database::begin_transaction --
     *     Start a new transaction.
     */
    kv_transaction_ptr begin_transaction(timestamp_t read_timestamp = k_timestamp_latest);

    /*
     * kv_database::remove_inactive_transaction --
     *     Remove a transaction from the list of active transactions. This should be only called
     *     from within the transaction's commit and rollback functions.
     */
    void remove_inactive_transaction(txn_id_t id);

    /*
     * kv_database::txn_snapshot --
     *     Create a transaction snapshot.
     */
    kv_transaction_snapshot_ptr txn_snapshot(txn_id_t do_not_exclude = k_txn_none);

    /*
     * kv_database::crash --
     *     Simulate crashing the database.
     */
    inline void
    crash()
    {
        restart(true);
    }

    /*
     * kv_database::restart --
     *     Simulate restarting the database - either a clean restart or crash and recovery.
     */
    void restart(bool crash = false);

    /*
     * kv_database::start --
     *     Simulate starting WiredTiger.
     */
    inline void
    start()
    {
        std::lock_guard lock_guard1(_tables_lock);
        std::lock_guard lock_guard2(_transactions_lock);
        std::lock_guard lock_guard3(_checkpoints_lock);
        start_nolock();
    }

    /*
     * kv_database::rollback_to_stable --
     *     Roll back the database to the latest stable timestamp. This simulates the runtime flavor
     *     of rollback to stable.
     */
    inline void
    rollback_to_stable()
    {
        rollback_to_stable(_stable_timestamp);
    }

protected:
    /*
     * kv_database::txn_snapshot --
     *     Create a transaction snapshot. Do not lock, because the caller already has a lock.
     */
    kv_transaction_snapshot_ptr txn_snapshot_nolock(txn_id_t do_not_exclude = k_txn_none);

    /*
     * kv_database::clear_nolock --
     *     Clear the contents of the database, assuming the relevant locks are already held.
     */
    void clear_nolock();

    /*
     * kv_database::rollback_all_nolock --
     *     Rollback all transactions, assuming the relevant locks are already held.
     */
    void rollback_all_nolock();

    /*
     * kv_database::rollback_to_stable --
     *     Roll back the database to the latest stable timestamp and transaction snapshot.
     */
    void rollback_to_stable(timestamp_t timestamp,
      kv_transaction_snapshot_ptr snapshot = kv_transaction_snapshot_ptr(nullptr));

    /*
     * kv_database::rollback_to_stable_nolock --
     *     Roll back the database to the latest stable timestamp and transaction snapshot, but
     *     without locking.
     */
    void rollback_to_stable_nolock(timestamp_t timestamp,
      kv_transaction_snapshot_ptr snapshot = kv_transaction_snapshot_ptr(nullptr));

    /*
     * kv_database::start_nolock --
     *     Simulate starting WiredTiger, assuming the locks are held.
     */
    void start_nolock();

private:
    /*
     * Locking order: If you need to acquire more than one lock at a time, acquire locks in the
     * order in which they are declared in this file to avoid deadlocks. For example, the tables
     * lock must be acquired before the transactions lock.
     */

    /*
     * Tables and transactions locks must be recursive, because simulating database restart needs to
     * acquire them before calling rollback to abort active transactions - and rollback also needs
     * to acquire both of these locks.
     */

    mutable std::recursive_mutex _tables_lock;
    std::unordered_map<std::string, kv_table_ptr> _tables; /* Maps table names to their objects. */

    mutable std::recursive_mutex _transactions_lock;
    txn_id_t _last_transaction_id;
    std::unordered_map<txn_id_t, kv_transaction_ptr> _active_transactions;

    mutable std::mutex _checkpoints_lock;
    std::unordered_map<std::string, kv_checkpoint_ptr> _checkpoints; /* Key: checkpoint name. */

    mutable std::mutex _timestamps_lock;
    timestamp_t _stable_timestamp;
};

} /* namespace model */
