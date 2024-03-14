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

#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include "model/driver/kv_workload.h"
#include "model/core.h"
#include "model/kv_database.h"
#include "model/kv_table.h"
#include "model/kv_transaction.h"
#include "wiredtiger.h"

namespace model {

/*
 * kv_workload_runner --
 *     The workload runner for the model.
 */
class kv_workload_runner {

public:
    /*
     * kv_workload_runner::kv_workload_runner --
     *     Create a new workload runner.
     */
    inline kv_workload_runner(kv_database &database) : _database(database) {}

    /*
     * kv_workload_runner::database --
     *     Get the database.
     */
    inline kv_database &
    database() const noexcept
    {
        return _database;
    }

    /*
     * kv_workload_runner::run --
     *     Run the workload in the model. Return the return codes of the workload operations.
     */
    inline std::vector<int>
    run(const kv_workload &workload)
    {
        std::vector<int> ret;
        for (size_t i = 0; i < workload.size(); i++)
            ret.push_back(run_operation(workload[i].operation));
        return ret;
    }

    /*
     * kv_workload_runner::run_operation --
     *     Run the given operation.
     */
    inline int
    run_operation(const operation::any &op)
    {
        int ret = WT_ERROR; /* So that Coverity does not complain. */
        std::visit([this, &ret](auto &&x) { ret = do_operation(x); }, op);
        return ret;
    }

protected:
    /*
     * kv_workload_runner::do_operation --
     *     Execute the given workload operation in the model.
     */
    int
    do_operation(const operation::begin_transaction &op)
    {
        add_transaction(op.txn_id, _database.begin_transaction());
        return 0;
    }

    /*
     * kv_workload_runner::do_operation --
     *     Execute the given workload operation in the model.
     */
    int
    do_operation(const operation::checkpoint &op)
    {
        _database.create_checkpoint(op.name.empty() ? nullptr : op.name.c_str());
        return 0;
    }

    /*
     * kv_workload_runner::do_operation --
     *     Execute the given workload operation in the model.
     */
    int
    do_operation(const operation::commit_transaction &op)
    {
        /* Remove the transaction first, so that the map has only uncommitted transactions. */
        remove_transaction(op.txn_id)->commit(op.commit_timestamp, op.durable_timestamp);
        return 0;
    }

    /*
     * kv_workload_runner::do_operation --
     *     Execute the given workload operation in the model.
     */
    int
    do_operation(const operation::crash &op)
    {
        (void)op;
        restart(true /* crash */);
        return 0;
    }

    /*
     * kv_workload_runner::do_operation --
     *     Execute the given workload operation in the model.
     */
    int
    do_operation(const operation::create_table &op)
    {
        kv_table_ptr table = _database.create_table(op.name);
        table->set_key_value_format(op.key_format, op.value_format);
        add_table(op.table_id, table);
        return 0;
    }

    /*
     * kv_workload_runner::do_operation --
     *     Execute the given workload operation in the model.
     */
    int
    do_operation(const operation::insert &op)
    {
        return table(op.table_id)->insert(transaction(op.txn_id), op.key, op.value);
    }

    /*
     * kv_workload_runner::do_operation --
     *     Execute the given workload operation in the model.
     */
    int
    do_operation(const operation::prepare_transaction &op)
    {
        transaction(op.txn_id)->prepare(op.prepare_timestamp);
        return 0;
    }

    /*
     * kv_workload_runner::do_operation --
     *     Execute the given workload operation in the model.
     */
    int
    do_operation(const operation::remove &op)
    {
        return table(op.table_id)->remove(transaction(op.txn_id), op.key);
    }

    /*
     * kv_workload_runner::do_operation --
     *     Execute the given workload operation in the model.
     */
    int
    do_operation(const operation::restart &op)
    {
        (void)op;
        restart();
        return 0;
    }

    /*
     * kv_workload_runner::do_operation --
     *     Execute the given workload operation in the model.
     */
    int
    do_operation(const operation::rollback_to_stable &op)
    {
        (void)op;
        _database.rollback_to_stable();
        return 0;
    }

    /*
     * kv_workload_runner::do_operation --
     *     Execute the given workload operation in the model.
     */
    int
    do_operation(const operation::rollback_transaction &op)
    {
        remove_transaction(op.txn_id)->rollback();
        return 0;
    }

    /*
     * kv_workload_runner::do_operation --
     *     Execute the given workload operation in the model.
     */
    int
    do_operation(const operation::set_commit_timestamp &op)
    {
        transaction(op.txn_id)->set_commit_timestamp(op.commit_timestamp);
        return 0;
    }

    /*
     * kv_workload_runner::do_operation --
     *     Execute the given workload operation in the model.
     */
    int
    do_operation(const operation::set_stable_timestamp &op)
    {
        _database.set_stable_timestamp(op.stable_timestamp);
        return 0;
    }

    /*
     * kv_workload_runner::do_operation --
     *     Execute the given workload operation in the model.
     */
    int
    do_operation(const operation::truncate &op)
    {
        return table(op.table_id)->truncate(transaction(op.txn_id), op.start, op.stop);
    }

    /*
     * kv_workload_runner::restart --
     *     Simulate database restart.
     */
    inline void
    restart(bool crash = false)
    {
        std::unique_lock lock(_transactions_lock);
        _transactions.clear();
        _database.restart(crash);
    }

    /*
     * kv_workload_runner::add_table --
     *     Add a table.
     */
    inline void
    add_table(table_id_t id, kv_table_ptr ptr)
    {
        std::unique_lock lock(_tables_lock);
        auto i = _tables.find(id);
        if (i != _tables.end())
            throw model_exception("A table with the given ID already exists");
        _tables.insert_or_assign(i, id, ptr);
    }

    /*
     * kv_workload_runner::table --
     *     Get the table.
     */
    inline kv_table_ptr
    table(table_id_t id) const
    {
        std::shared_lock lock(_tables_lock);
        auto i = _tables.find(id);
        if (i == _tables.end())
            throw model_exception("A table with the given ID does not exist");
        return i->second;
    }

    /*
     * kv_workload_runner::add_transaction --
     *     Add a transaction.
     */
    inline void
    add_transaction(txn_id_t id, kv_transaction_ptr ptr)
    {
        std::unique_lock lock(_transactions_lock);
        auto i = _transactions.find(id);
        if (i != _transactions.end())
            throw model_exception("A transaction with the given ID already exists");
        _transactions.insert_or_assign(i, id, ptr);
    }

    /*
     * kv_workload_runner::remove_transaction --
     *     Remove a transaction.
     */
    inline kv_transaction_ptr
    remove_transaction(txn_id_t id)
    {
        std::unique_lock lock(_transactions_lock);
        auto i = _transactions.find(id);
        if (i == _transactions.end())
            throw model_exception("A transaction with the given ID does not already exist");
        kv_transaction_ptr txn = i->second;
        _transactions.erase(i);
        return txn;
    }

    /*
     * kv_workload_runner::transaction --
     *     Get the transaction.
     */
    inline kv_transaction_ptr
    transaction(txn_id_t id) const
    {
        std::shared_lock lock(_transactions_lock);
        auto i = _transactions.find(id);
        if (i == _transactions.end())
            throw model_exception("A transaction with the given ID does not exist");
        return i->second;
    }

private:
    kv_database &_database;

    mutable std::shared_mutex _tables_lock;
    std::unordered_map<table_id_t, kv_table_ptr> _tables;

    mutable std::shared_mutex _transactions_lock;
    std::unordered_map<txn_id_t, kv_transaction_ptr> _transactions;
};

} /* namespace model */
