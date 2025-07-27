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

#include <algorithm>
#include <iostream>

extern "C" {
#include "wt_internal.h"
}

#include "model/kv_database.h"
#include "model/kv_transaction.h"
#include "model/util.h"

namespace model {

/*
 * kv_database::_default_config --
 *     The default database configuration.
 */
const kv_database_config kv_database::_default_config;

/*
 * kv_database_config::kv_database_config --
 *     Create the default database configuration.
 */
kv_database_config::kv_database_config()
{
    disaggregated = false;
    leader = true;
}

/*
 * kv_database_config::from_string --
 *     Create the configuration from a string. Throw an exception on error.
 */
kv_database_config
kv_database_config::from_string(const std::string &config)
{
    kv_database_config r;

    config_map m = config_map::from_string(config);
    std::vector<std::string> keys = m.keys();
    for (std::string &k : keys) {
        if (k == "disaggregated")
            r.disaggregated = m.get_bool(k.c_str());
        else if (k == "leader")
            r.leader = m.get_bool(k.c_str());
        else
            throw std::runtime_error("Invalid database configuration key: " + k);
    }

    return r;
}

/*
 * kv_database::create_table --
 *     Create and return a new table. Throw an exception if the name is not unique.
 */
kv_table_ptr
kv_database::create_table(const char *name, const kv_table_config &config)
{
    std::string s = std::string(name);
    std::lock_guard lock_guard(_tables_lock);

    auto i = _tables.find(s);
    if (i != _tables.end())
        throw model_exception("Table already exists: " + s);

    kv_table_ptr table = std::make_shared<kv_table>(*this, name, config);
    _tables[s] = table;
    return table;
}

/*
 * kv_database::create_checkpoint --
 *     Create a checkpoint. Throw an exception if the name is not unique.
 */
kv_checkpoint_ptr
kv_database::create_checkpoint(const char *name)
{
    return create_checkpoint(
      name, txn_snapshot(k_txn_none, true), _oldest_timestamp, _stable_timestamp);
}

/*
 * kv_database::create_checkpoint --
 *     Create a checkpoint from custom metadata. Throw an exception if the name is not unique.
 */
kv_checkpoint_ptr
kv_database::create_checkpoint(const char *name, kv_transaction_snapshot_ptr snapshot,
  timestamp_t oldest_timestamp, timestamp_t stable_timestamp)
{
    std::lock_guard lock_guard1(_tables_lock);
    std::lock_guard lock_guard(_checkpoints_lock);
    return create_checkpoint_nolock(name, std::move(snapshot), oldest_timestamp, stable_timestamp);
}

/*
 * kv_database::create_checkpoint --
 *     Create a checkpoint from custom metadata. Throw an exception if the name is not unique.
 *     Assume the relevant locks are held.
 */
kv_checkpoint_ptr
kv_database::create_checkpoint_nolock(const char *name, kv_transaction_snapshot_ptr snapshot,
  timestamp_t oldest_timestamp, timestamp_t stable_timestamp)
{
    /* Requires the table and the checkpoint lock. */

    /* Use the default checkpoint name, if it is not specified. */
    if (name == nullptr)
        name = WT_CHECKPOINT;
    std::string ckpt_name = name;

    /* We can overwrite the default checkpoint, but not the others without deleting them first. */
    if (ckpt_name != WT_CHECKPOINT) {
        auto i = _checkpoints.find(ckpt_name);
        if (i != _checkpoints.end())
            throw model_exception("Checkpoint already exists: " + ckpt_name);
    }

    /* Remember the oldest timestamp only if the stable timestamp is set. */
    if (stable_timestamp == k_timestamp_none)
        oldest_timestamp = k_timestamp_none;

    /* Get the highest recno for each FLCS table. */
    std::map<std::string, uint64_t> highest_recnos;
    for (auto &p : _tables)
        if (p.second->type() == kv_table_type::column_fix)
            highest_recnos[p.first] = p.second->highest_recno();

    /* Create the checkpoint. */
    kv_checkpoint_ptr ckpt = std::make_shared<kv_checkpoint>(
      name, snapshot, oldest_timestamp, stable_timestamp, std::move(highest_recnos));

    /* Remember it. */
    _checkpoints[ckpt_name] = ckpt;
    return ckpt;
}

/*
 * kv_database::checkpoint --
 *     Get the checkpoint. Throw an exception if it does not exist.
 */
kv_checkpoint_ptr
kv_database::checkpoint(const char *name)
{
    std::string ckpt_name = name != nullptr ? name : WT_CHECKPOINT;
    return checkpoint(ckpt_name);
}

/*
 * kv_database::begin_transaction --
 *     Start a new transaction.
 */
kv_transaction_ptr
kv_database::begin_transaction(timestamp_t read_timestamp)
{
    std::lock_guard lock_guard(_transactions_lock);

    txn_id_t id = ++_last_transaction_id;
    kv_transaction_ptr txn =
      std::make_shared<kv_transaction>(*this, id, txn_snapshot_nolock(), read_timestamp);

    _active_transactions[id] = txn;
    return txn;
}

/*
 * kv_database::remove_inactive_transaction --
 *     Remove a transaction from the list of active transactions. This should be only called from
 *     within the transaction's commit and rollback functions.
 */
void
kv_database::remove_inactive_transaction(txn_id_t id)
{
    std::lock_guard lock_guard(_transactions_lock);
    auto i = _active_transactions.find(id);
    if (i != _active_transactions.end()) {
        kv_transaction_state state = i->second->state();
        if (state != kv_transaction_state::committed && state != kv_transaction_state::rolled_back)
            throw model_exception("Attempting to remove an active transaction");
        _active_transactions.erase(i);
    }
}

/*
 * kv_database::txn_snapshot --
 *     Create a transaction snapshot.
 */
kv_transaction_snapshot_ptr
kv_database::txn_snapshot(txn_id_t do_not_exclude, bool is_checkpoint)
{
    std::lock_guard lock_guard(_transactions_lock);
    return txn_snapshot_nolock(do_not_exclude, is_checkpoint);
}

/*
 * kv_database::txn_snapshot --
 *     Create a transaction snapshot. Do not lock, because the caller already has a lock.
 */
kv_transaction_snapshot_ptr
kv_database::txn_snapshot_nolock(txn_id_t do_not_exclude, bool is_checkpoint)
{
    std::unordered_set<txn_id_t> active_txn_ids;
    for (auto &p : _active_transactions) {
        if (p.first == do_not_exclude)
            continue;
        kv_transaction_state state = p.second->state();

        if (state == kv_transaction_state::prepared) {
            /*
             * Checkpoints should ignore prepared transactions. They are not restored during
             * recovery.
             */
            if (is_checkpoint)
                active_txn_ids.insert(p.first);
        } else if (state == kv_transaction_state::in_progress) {
            /*
             * Outside of checkpoints, prepared transactions can be visible under some
             * circumstances, and the specific read / update / etc operations should handle the
             * possibility of prepared transactions.
             */
            active_txn_ids.insert(p.first);
        }
    }
    return std::make_shared<kv_transaction_snapshot_by_exclusion>(
      _last_transaction_id, std::move(active_txn_ids));
}

/*
 * kv_database::clear_nolock --
 *     Clear the contents of the database, assuming the relevant locks are already held.
 */
void
kv_database::clear_nolock()
{
    /* Requires: tables lock, transactions lock. */

    /* Reset the database's timestamps. */
    _oldest_timestamp = k_timestamp_none;
    _stable_timestamp = k_timestamp_none;

    /*
     * Roll back all active transactions. We cannot just clear the table of active transactions, as
     * that would result in a memory leak due to circular dependencies between updates and
     * transactions.
     */
    rollback_all_nolock();

    /* Clear the tables. */
    for (auto &p : _tables)
        p.second->clear();
}

/*
 * kv_database::rollback_all_nolock --
 *     Rollback all transactions, assuming the relevant locks are already held.
 */
void
kv_database::rollback_all_nolock()
{
    /*
     * Fail all active transactions. We have to do this in two steps, as the map of active
     * transactions will be modified during the calls to rollback.
     */
    std::vector<kv_transaction_ptr> to_rollback;
    for (auto &p : _active_transactions)
        to_rollback.push_back(p.second);
    for (auto &p : to_rollback)
        p->rollback();
    assert(_active_transactions.empty());
}

/*
 * kv_database::restart --
 *     Simulate restarting the database - either a clean restart or crash and recovery.
 */
void
kv_database::restart(bool crash)
{
    std::lock_guard lock_guard1(_tables_lock);
    std::lock_guard lock_guard2(_transactions_lock);

    /* Fail all active transactions. */
    rollback_all_nolock();

    /* If we are not crashing, create a checkpoint. */
    if (!crash)
        create_checkpoint();

    /* Start WiredTiger. */
    std::lock_guard lock_guard3(_checkpoints_lock);
    start_nolock();
}

/*
 * kv_database::rollback_to_stable --
 *     Roll back the database to the latest stable timestamp and transaction snapshot.
 */
void
kv_database::rollback_to_stable(timestamp_t timestamp, kv_transaction_snapshot_ptr snapshot)
{
    std::lock_guard lock_guard1(_tables_lock);
    std::lock_guard lock_guard2(_transactions_lock);
    std::lock_guard lock_guard3(_checkpoints_lock);

    rollback_to_stable_nolock(timestamp, std::move(snapshot));
}

/*
 * kv_database::rollback_to_stable_nolock --
 *     Roll back the database to the latest stable timestamp and transaction snapshot, but without
 *     locking.
 */
void
kv_database::rollback_to_stable_nolock(timestamp_t timestamp, kv_transaction_snapshot_ptr snapshot)
{
    if (!_active_transactions.empty())
        throw model_exception("There are active transactions");

    /* If the stable timestamp is not set, do not roll back based on it. */
    if (timestamp == k_timestamp_none)
        timestamp = k_timestamp_latest;

    for (auto &p : _tables)
        p.second->rollback_to_stable(timestamp, snapshot);

    /* Force a checkpoint. */
    create_checkpoint_nolock(WT_CHECKPOINT, txn_snapshot(), _oldest_timestamp, _stable_timestamp);
}

/*
 * kv_database::start_nolock --
 *     Simulate starting WiredTiger, assuming the locks are held.
 */
void
kv_database::start_nolock()
{
    /* If there is no nameless checkpoint, we have an empty table. */
    auto i = _checkpoints.find(std::string(WT_CHECKPOINT));
    if (i == _checkpoints.end()) {
        clear_nolock();
        return;
    }

    /* Otherwise recover using rollback to stable using the checkpoint. */
    kv_checkpoint_ptr ckpt = i->second;

    /* Restore the database's oldest and stable timestamps. */
    _oldest_timestamp = ckpt->oldest_timestamp();
    timestamp_t t = ckpt->stable_timestamp();
    _stable_timestamp = t;

    /* If the checkpoint does not have a stable timestamp, do not use it during RTS. */
    if (t == k_timestamp_none)
        t = k_timestamp_latest;

    /* Restore highest recnos for each FLCS. */
    for (auto &p : ckpt->highest_recnos())
        table_nolock(p.first)->truncate_recnos_after(p.second);

    /* Run RTS, even for disaggregated storage, which is a way to simulate precise checkpoints. */
    rollback_to_stable_nolock(t, ckpt->snapshot());
}

} /* namespace model */
