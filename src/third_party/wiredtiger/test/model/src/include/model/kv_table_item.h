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

#include <deque>
#include <memory>
#include <mutex>

#include "model/data_value.h"
#include "model/kv_checkpoint.h"
#include "model/kv_update.h"

namespace model {

/*
 * kv_table_item --
 *     The value part of a key-value pair, together with its metadata and previous versions.
 */
class kv_table_item {

public:
    /*
     * kv_table_item::kv_table_item --
     *     Create a new instance.
     */
    inline kv_table_item() noexcept {}

    /*
     * kv_table_item::add_update --
     *     Add an update. Throw exception on error.
     */
    void add_update(std::shared_ptr<kv_update> update, bool must_exist, bool must_not_exist);

    /*
     * kv_table_item::contains_any --
     *     Check whether the table contains the given value. If there are multiple values associated
     *     with the given timestamp, return true if any of them match.
     */
    inline bool
    contains_any(const data_value &value, timestamp_t timestamp = k_timestamp_latest) const
    {
        return contains_any(value, kv_transaction_snapshot_ptr(nullptr), timestamp);
    }

    /*
     * kv_table_item::contains_any --
     *     Check whether the table contains the given value. If there are multiple values associated
     *     with the given timestamp, return true if any of them match.
     */
    inline bool
    contains_any(kv_checkpoint_ptr ckpt, const data_value &value) const
    {
        if (!ckpt)
            throw model_exception("Null checkpoint");
        timestamp_t timestamp = ckpt->stable_timestamp() != k_timestamp_none ?
          ckpt->stable_timestamp() :
          k_timestamp_latest;
        return contains_any(value, ckpt->snapshot(), timestamp, timestamp);
    }

    /*
     * kv_table_item::exists --
     *     Check whether the latest value exists.
     */
    bool exists() const;

    /*
     * kv_table_item::exists --
     *     Check whether the latest value exists in the given checkpoint.
     */
    bool exists(kv_checkpoint_ptr checkpoint) const;

    /*
     * kv_table_item::exists_opt --
     *     Check whether the latest value exists, using the checkpoint if provided.
     */
    inline bool
    exists_opt(kv_checkpoint_ptr checkpoint) const
    {
        return checkpoint ? exists(checkpoint) : exists();
    }

    /*
     * kv_table_item::get --
     *     Get the corresponding value. Return NONE if not found. Throw an exception on error.
     */
    inline data_value
    get(timestamp_t timestamp = k_timestamp_latest) const
    {
        return get(kv_transaction_snapshot_ptr(nullptr), k_txn_none, timestamp);
    }

    /*
     * kv_table_item::get --
     *     Get the corresponding value. Return NONE if not found. Throw an exception on error.
     */
    inline data_value
    get(kv_checkpoint_ptr ckpt, timestamp_t timestamp = k_timestamp_latest) const
    {
        if (!ckpt)
            throw model_exception("Null checkpoint");

        /* Get the stable (checkpoint) timestamp, if not overridden by the caller. */
        if (timestamp == k_timestamp_latest)
            timestamp = ckpt->stable_timestamp() != k_timestamp_none ? ckpt->stable_timestamp() :
                                                                       k_timestamp_latest;

        /*
         * When using checkpoint cursors, we need to compare the stable timestamp against the
         * durable timestamp, not the commit timestamp.
         */
        return get(ckpt->snapshot(), k_txn_none, timestamp, timestamp);
    }

    /*
     * kv_table_item::get --
     *     Get the corresponding value. Return NONE if not found. Throw an exception on error.
     */
    inline data_value
    get(kv_transaction_ptr txn) const
    {
        if (!txn)
            throw model_exception("Null transaction");
        return get(txn->snapshot(), txn->id(), txn->read_timestamp());
    }

    /*
     * kv_table_item::get_latest --
     *     Get the corresponding value, but ignore the transaction's read timestamp. Return NONE if
     *     not found. Throw an exception on error.
     */
    inline data_value
    get_latest(kv_transaction_ptr txn) const
    {
        if (!txn)
            throw model_exception("Null transaction");
        return get(txn->snapshot(), txn->id(), k_timestamp_latest);
    }

    /*
     * kv_table_item::fix_timestamps --
     *     Fix the commit and durable timestamps for the corresponding update. We need to do this,
     *     because WiredTiger transaction API specifies the commit timestamp after performing the
     *     operations, not before.
     */
    void fix_timestamps(
      txn_id_t txn_id, timestamp_t commit_timestamp, timestamp_t durable_timestamp);

    /*
     * kv_table_item::has_prepared --
     *     Check whether the item has any prepared updates for the given timestamp.
     */
    bool has_prepared(timestamp_t timestamp) const;

    /*
     * kv_table_item::rollback_to_stable --
     *     Roll back the table item to the latest stable timestamp and transaction snapshot.
     */
    void rollback_to_stable(timestamp_t timestamp, kv_transaction_snapshot_ptr snapshot);

    /*
     * kv_table_item::rollback_updates --
     *     Roll back updates of an aborted transaction.
     */
    void rollback_updates(txn_id_t txn_id);

protected:
    /*
     * kv_table_item::add_update_nolock --
     *     Add an update but without taking a lock (this assumes the caller has it). Throw an
     *     exception on error.
     */
    void add_update_nolock(std::shared_ptr<kv_update> update, bool must_exist, bool must_not_exist);

    /*
     * kv_table_item::fail_with_rollback --
     *     Fail the given update and throw an exception indicating rollback.
     */
    void fail_with_rollback(std::shared_ptr<kv_update> update);

    /*
     * kv_table_item::contains_any --
     *     Check whether the table contains the given value. If there are multiple values associated
     *     with the given timestamp, return true if any of them match.
     */
    bool contains_any(const data_value &value, kv_transaction_snapshot_ptr txn_snapshot,
      timestamp_t read_timestamp, timestamp_t stable_timestamp = k_timestamp_latest) const;

    /*
     * kv_table_item::get --
     *     Get the corresponding value. Return NONE if not found. Throw an exception on error.
     */
    data_value get(kv_transaction_snapshot_ptr txn_snapshot, txn_id_t txn_id,
      timestamp_t read_timestamp, timestamp_t stable_timestamp = k_timestamp_latest) const;

    /*
     * kv_table_item::has_prepared_nolock --
     *     Check whether the item has any prepared updates for the given timestamp, but without
     *     taking a lock.
     */
    bool has_prepared_nolock(timestamp_t timestamp) const;

private:
    mutable std::mutex _lock;
    std::deque<std::shared_ptr<kv_update>> _updates; /* sorted list of updates */
};

} /* namespace model */
