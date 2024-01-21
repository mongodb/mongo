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

#ifndef MODEL_DRIVER_KV_WORKLOAD_H
#define MODEL_DRIVER_KV_WORKLOAD_H

#include <deque>
#include <variant>
#include "model/core.h"
#include "model/data_value.h"
#include "model/kv_database.h"

namespace model {

using table_id_t = int;

/*
 * op --
 *     The namespace for all workload operations.
 */
namespace operation {

/*
 * begin_transaction --
 *     A representation of this workload operation.
 */
struct begin_transaction {
    txn_id_t txn_id; /* This will be the public ID. */

    /*
     * begin_transaction::begin_transaction --
     *     Create the operation.
     */
    inline begin_transaction(txn_id_t txn_id) : txn_id(txn_id) {}
};

/*
 * checkpoint --
 *     A representation of this workload operation.
 */
struct checkpoint {
    std::string name;

    /*
     * checkpoint::checkpoint --
     *     Create the operation.
     */
    inline checkpoint(const char *name = nullptr) : name(name == nullptr ? "" : name) {}
};

/*
 * commit_transaction --
 *     A representation of this workload operation.
 */
struct commit_transaction {
    txn_id_t txn_id;
    timestamp_t commit_timestamp;
    timestamp_t durable_timestamp;

    /*
     * commit_transaction::commit_transaction --
     *     Create the operation.
     */
    inline commit_transaction(txn_id_t txn_id, timestamp_t commit_timestamp = k_timestamp_none,
      timestamp_t durable_timestamp = k_timestamp_none)
        : txn_id(txn_id), commit_timestamp(commit_timestamp), durable_timestamp(durable_timestamp)
    {
    }
};

/*
 * create_table --
 *     A representation of this workload operation.
 */
struct create_table {
    table_id_t table_id; /* This will be the table's public ID. */
    std::string name;
    std::string key_format;
    std::string value_format;

    /*
     * create_table::create_table --
     *     Create the operation.
     */
    inline create_table(
      table_id_t table_id, const char *name, const char *key_format, const char *value_format)
        : table_id(table_id), name(name), key_format(key_format), value_format(value_format)
    {
    }
};

/*
 * insert --
 *     A representation of this workload operation.
 */
struct insert {
    table_id_t table_id;
    txn_id_t txn_id;
    data_value key;
    data_value value;

    /*
     * insert::insert --
     *     Create the operation.
     */
    inline insert(
      table_id_t table_id, txn_id_t txn_id, const data_value &key, const data_value &value)
        : table_id(table_id), txn_id(txn_id), key(key), value(value)
    {
    }
};

/*
 * prepare_transaction --
 *     A representation of this workload operation.
 */
struct prepare_transaction {
    txn_id_t txn_id;
    timestamp_t prepare_timestamp;

    /*
     * prepare_transaction::prepare_transaction --
     *     Create the operation.
     */
    inline prepare_transaction(txn_id_t txn_id, timestamp_t prepare_timestamp)
        : txn_id(txn_id), prepare_timestamp(prepare_timestamp)
    {
    }
};

/*
 * remove --
 *     A representation of this workload operation.
 */
struct remove {
    table_id_t table_id;
    txn_id_t txn_id;
    data_value key;

    /*
     * remove::remove --
     *     Create the operation.
     */
    inline remove(table_id_t table_id, txn_id_t txn_id, const data_value &key)
        : table_id(table_id), txn_id(txn_id), key(key)
    {
    }
};

/*
 * restart --
 *     A representation of this workload operation.
 */
struct restart {

    /*
     * restart::restart --
     *     Create the operation.
     */
    inline restart() {}
};

/*
 * rollback_to_stable --
 *     A representation of this workload operation.
 */
struct rollback_to_stable {

    /*
     * rollback_to_stable::rollback_to_stable --
     *     Create the operation.
     */
    inline rollback_to_stable() {}
};

/*
 * rollback_transaction --
 *     A representation of this workload operation.
 */
struct rollback_transaction {
    txn_id_t txn_id;

    /*
     * rollback_transaction::rollback_transaction --
     *     Create the operation.
     */
    inline rollback_transaction(txn_id_t txn_id) : txn_id(txn_id) {}
};

/*
 * set_commit_timestamp --
 *     A representation of this workload operation.
 */
struct set_commit_timestamp {
    txn_id_t txn_id;
    timestamp_t commit_timestamp;

    /*
     * set_commit_timestamp::set_commit_timestamp --
     *     Create the operation.
     */
    inline set_commit_timestamp(txn_id_t txn_id, timestamp_t commit_timestamp)
        : txn_id(txn_id), commit_timestamp(commit_timestamp)
    {
    }
};

/*
 * set_stable_timestamp --
 *     A representation of this workload operation.
 */
struct set_stable_timestamp {
    timestamp_t stable_timestamp;

    /*
     * set_stable_timestamp::set_stable_timestamp --
     *     Create the operation.
     */
    inline set_stable_timestamp(timestamp_t stable_timestamp) : stable_timestamp(stable_timestamp)
    {
    }
};

/*
 * truncate --
 *     A representation of this workload operation.
 */
struct truncate {
    table_id_t table_id;
    txn_id_t txn_id;
    data_value start;
    data_value stop;

    /*
     * truncate::truncate --
     *     Create the operation.
     */
    inline truncate(
      table_id_t table_id, txn_id_t txn_id, const data_value &start, const data_value &stop)
        : table_id(table_id), txn_id(txn_id), start(start), stop(stop)
    {
    }
};

/*
 * any --
 *     Any workload operation.
 */
using any = std::variant<begin_transaction, checkpoint, commit_transaction, create_table, insert,
  prepare_transaction, remove, restart, rollback_to_stable, rollback_transaction,
  set_commit_timestamp, set_stable_timestamp, truncate>;

} /* namespace operation */

/*
 * kv_workload --
 *     A workload representation for a key-value database.
 */
class kv_workload {

public:
    /*
     * kv_workload::kv_workload --
     *     Create a new workload.
     */
    inline kv_workload() {}

    /*
     * kv_workload::operator<< --
     *     Add an operation to the workload.
     */
    inline kv_workload &
    operator<<(const operation::any &op)
    {
        _operations.push_back(op);
        return *this;
    }

    /*
     * kv_workload::operator<< --
     *     Add an operation to the workload.
     */
    inline kv_workload &
    operator<<(operation::any &&op)
    {
        _operations.push_back(std::move(op));
        return *this;
    }

    /*
     * kv_workload::run --
     *     Run the workload in the model.
     */
    void run(kv_database &database) const;

    /*
     * kv_workload::run_in_wiredtiger --
     *     Run the workload in WiredTiger.
     */
    void run_in_wiredtiger(const char *home, const char *connection_config) const;

private:
    std::deque<operation::any> _operations;
};

} /* namespace model */
#endif
