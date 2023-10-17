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

#include "model/kv_database.h"
#include "model/kv_table.h"
#include "model/kv_transaction.h"

namespace model {

/*
 * kv_transaction::add_update --
 *     Add update. The update should be at this point already incorporated in the table.
 */
void
kv_transaction::add_update(
  kv_table &table, const data_value &key, std::shared_ptr<kv_update> update)
{
    std::lock_guard lock_guard(_lock);
    std::shared_ptr<kv_transaction_update> txn_update =
      std::make_shared<kv_transaction_update>(table.name(), key, update);

    _updates.push_back(txn_update);
    if (_commit_timestamp == k_initial_commit_timestamp)
        _nontimestamped_updates.push_back(txn_update);
}

/*
 * kv_transaction::commit --
 *     Commit the transaction.
 */
void
kv_transaction::commit(timestamp_t commit_timestamp)
{
    std::lock_guard lock_guard(_lock);

    if (_failed && !_updates.empty())
        throw model_exception("Failed transaction requires rollback");

    /* Fix commit timestamps. */
    for (auto &u : _nontimestamped_updates)
        _database.table(u->table_name())->fix_commit_timestamp(u->key(), _id, commit_timestamp);

    /* Mark the transaction as committed. */
    _state.store(kv_transaction_state::committed, std::memory_order_release);

    /* Remove the transaction object from the updates to save memory. */
    for (auto &u : _updates)
        u->update()->remove_txn();

    /* Remove from the list of active transactions. */
    _database.remove_inactive_transaction(_id);
}

/*
 * kv_transaction::reset_snapshot --
 *     Reset the transaction snapshot.
 */
void
kv_transaction::reset_snapshot()
{
    std::lock_guard lock_guard(_lock);
    _snapshot = _database.txn_snapshot(_id);
}

/*
 * kv_transaction::rollback --
 *     Abort the transaction.
 */
void
kv_transaction::rollback()
{
    std::lock_guard lock_guard(_lock);

    /* Mark the transaction as rolled back. */
    _state.store(kv_transaction_state::rolled_back, std::memory_order_release);

    /* Remove updates. */
    for (auto &u : _updates)
        _database.table(u->table_name())->rollback_updates(u->key(), _id);

    /* Remove from the list of active transactions. */
    _database.remove_inactive_transaction(_id);
}

/*
 * kv_transaction::set_timestamp --
 *     Set the timestamp for all subsequent updates.
 */
void
kv_transaction::set_timestamp(timestamp_t commit_timestamp)
{
    std::lock_guard lock_guard(_lock);
    _commit_timestamp = commit_timestamp;
}

} /* namespace model */
