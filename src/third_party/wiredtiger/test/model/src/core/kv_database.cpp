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
#include "model/kv_transaction.h"

namespace model {

/*
 * kv_database::create_table --
 *     Create and return a new table. Throw an exception if the name is not unique.
 */
kv_table_ptr
kv_database::create_table(const char *name)
{
    std::string s = std::string(name);
    std::lock_guard lock_guard(_tables_lock);

    auto i = _tables.find(s);
    if (i != _tables.end())
        throw model_exception(std::string("Table already exists: ") + s);

    kv_table_ptr table = std::make_shared<kv_table>(name);
    _tables[s] = table;
    return table;
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
      std::make_shared<kv_transaction>(*this, id, std::move(txn_snapshot_nolock()), read_timestamp);

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
kv_transaction_snapshot
kv_database::txn_snapshot(txn_id_t do_not_exclude)
{
    std::lock_guard lock_guard(_transactions_lock);
    return txn_snapshot_nolock(do_not_exclude);
}

/*
 * kv_database::txn_snapshot --
 *     Create a transaction snapshot. Do not lock, because the caller already has a lock.
 */
kv_transaction_snapshot
kv_database::txn_snapshot_nolock(txn_id_t do_not_exclude)
{
    std::unordered_set<txn_id_t> active_txn_ids;
    for (auto &p : _active_transactions) {
        if (p.first == do_not_exclude)
            continue;
        kv_transaction_state state = p.second->state();
        if (state != kv_transaction_state::committed && state != kv_transaction_state::rolled_back)
            active_txn_ids.insert(p.first);
    }
    return kv_transaction_snapshot{_last_transaction_id, std::move(active_txn_ids)};
}

} /* namespace model */
