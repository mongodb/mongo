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

#ifndef MODEL_KV_DATABASE_H
#define MODEL_KV_DATABASE_H

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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
    inline kv_database() : _last_transaction_id(k_txn_none) {}

    /*
     * kv_database::create_table --
     *     Create and return a new table. Throw an exception if the name is not unique.
     */
    inline kv_table_ptr
    create_table(const std::string &name)
    {
        return create_table(name.c_str());
    }

    /*
     * kv_database::create_table --
     *     Create and return a new table. Throw an exception if the name is not unique.
     */
    kv_table_ptr create_table(const char *name);

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
            throw model_exception(std::string("No such table: ") + name);
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
    kv_transaction_snapshot txn_snapshot(txn_id_t do_not_exclude = k_txn_none);

protected:
    /*
     * kv_database::txn_snapshot --
     *     Create a transaction snapshot. Do not lock, because the caller already has a lock.
     */
    kv_transaction_snapshot txn_snapshot_nolock(txn_id_t do_not_exclude = k_txn_none);

private:
    mutable std::mutex _tables_lock;
    std::unordered_map<std::string, kv_table_ptr> _tables;

    mutable std::mutex _transactions_lock;
    txn_id_t _last_transaction_id;
    std::unordered_map<txn_id_t, kv_transaction_ptr> _active_transactions;
};

} /* namespace model */
#endif
