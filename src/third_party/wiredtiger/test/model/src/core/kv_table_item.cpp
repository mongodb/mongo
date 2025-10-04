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
#include <cassert>
#include <iostream>
#include <list>
#include <sstream>

#include "model/kv_table.h"
#include "model/kv_table_item.h"
#include "wiredtiger.h"

namespace model {

/*
 * kv_table_item::add_update --
 *     Add an update. Throw exception on error.
 */
void
kv_table_item::add_update(std::shared_ptr<kv_update> update, bool must_exist, bool must_not_exist)
{
    std::lock_guard lock_guard(_lock);
    add_update_nolock(std::move(update), must_exist, must_not_exist);
}

/*
 * kv_table_item::add_update_nolock --
 *     Add an update but without taking a lock (this assumes the caller has it). Throw an exception
 *     on error.
 */
void
kv_table_item::add_update_nolock(
  std::shared_ptr<kv_update> update, bool must_exist, bool must_not_exist)
{
    kv_update::commit_timestamp_comparator cmp;

    /* If this is a non-timestamped update, there cannot be existing timestamped updates. */
    if (update->global())
        if (!_updates.empty() && !_updates.back()->global())
            throw wiredtiger_exception(EINVAL);

    /* Check for transaction conflicts. */
    kv_transaction_ptr txn = update->txn();
    if (txn) {
        /*
         * Check whether the update chain has an update that is not included in this transaction's
         * snapshot, or if it has an uncommitted update (regardless of whether it is included in its
         * transaction snapshot.
         */
        for (auto &u : _updates) {
            switch (u->txn_state()) {
            case kv_transaction_state::in_progress:
                /* Can't conflict with self. */
                if (u->txn_id() == txn->id()) {
                    /*
                     * Cannot update a key with a more recent timestamp. If we do this, WiredTiger
                     * would abort during commit.
                     */
                    if (u->commit_timestamp() > update->commit_timestamp())
                        throw wiredtiger_abort_exception(
                          "Updating a key with a more recent timestamp");
                    break;
                }
                fail_with_rollback(update);
            case kv_transaction_state::committed:
            case kv_transaction_state::prepared:
                if (!txn->visible_update(*u))
                    fail_with_rollback(update);
                break;
            case kv_transaction_state::rolled_back:
            default:
                break;
            }
        }
    }

    /* Position the update. */
    auto i = std::upper_bound(_updates.begin(), _updates.end(), update.get(), cmp);

    /* If need be, fail if the key does not exist. */
    if (must_exist) {
        if (_updates.empty())
            throw wiredtiger_exception(WT_NOTFOUND);

        auto j = i;
        if (j == _updates.begin() || (*(--j))->value() == NONE)
            throw wiredtiger_exception(WT_NOTFOUND);
    }

    /* If need be, fail if the key exists. */
    if (must_not_exist && !_updates.empty()) {
        auto j = i;
        if (j != _updates.begin() && (*(--j))->value() != NONE)
            throw wiredtiger_exception(WT_DUPLICATE_KEY);
    }

    /* Insert. */
    _updates.insert(i, update);
}

/*
 * kv_table_item::fail_with_rollback --
 *     Fail the given update and throw an exception indicating rollback.
 */
void
kv_table_item::fail_with_rollback(std::shared_ptr<kv_update> update)
{
    kv_transaction_ptr txn = update->txn();
    if (txn)
        txn->fail();
    throw wiredtiger_exception(WT_ROLLBACK);
}

/*
 * kv_table_item::contains_any --
 *     Check whether the table contains the given value. If there are multiple value associated with
 *     the given timestamp, return true if any of them match.
 */
bool
kv_table_item::contains_any(const data_value &value, kv_transaction_snapshot_ptr txn_snapshot,
  timestamp_t read_timestamp, timestamp_t stable_timestamp) const
{
    std::lock_guard lock_guard(_lock);
    kv_update::commit_timestamp_comparator cmp;

    /* Position the cursor on the update that is right after the provided timestamp. */
    auto i = std::upper_bound(_updates.begin(), _updates.end(), read_timestamp, cmp);

    /*
     * If we are positioned at the beginning of the list, there are no visible updates given the
     * provided timestamp (i.e., with timestamp that is smaller than or equal to the provided
     * timestamp).
     */
    if (i == _updates.begin())
        return false;

    /*
     * Position the iterator to the latest visible update, if we consider only on the read
     * timestamp.
     */
    i--;

    /* Skip any updates that are not visible due to the stable timestamp or the snapshot. */
    auto update_visible = [&](const std::shared_ptr<kv_update> &u) -> bool {
        return (!txn_snapshot || txn_snapshot->contains(*u)) &&
          u->durable_timestamp() <= stable_timestamp;
    };
    for (;;) {
        const std::shared_ptr<kv_update> &u = *i;

        /* Check the update's visibility. */
        if (update_visible(u))
            break;

        /* Otherwise go to the previous update (unless we are already at the beginning). */
        if (i == _updates.begin())
            return false; /* No more updates - we are done. */
        i--;
    }

    /*
     * Now check all updates with the same commit timestamp. If the most recent update is not
     * implicit (i.e., added explicitly by the user as opposed to being added automatically when
     * filling in zeros in FLCS), then check only explicit updates.
     */
    timestamp_t t = (*i)->commit_timestamp();
    bool implicit = (*i)->implicit();
    while ((*i)->commit_timestamp() == t && (implicit || !(*i)->implicit())) {
        const std::shared_ptr<kv_update> &u = *i;

        /* Found one! */
        if (update_visible(u) && u->value() == value)
            return true;

        /* Otherwise go to the previous update (unless we are already at the beginning). */
        if (i == _updates.begin())
            break;
        i--;
    }

    return false;
}

/*
 * kv_table_item::exists --
 *     Check whether the latest value exists.
 */
bool
kv_table_item::exists() const
{
    return get(k_timestamp_latest) != NONE;
}

/*
 * kv_table_item::exists --
 *     Check whether the latest value exists in the given checkpoint.
 */
bool
kv_table_item::exists(kv_checkpoint_ptr checkpoint) const
{
    return get(std::move(checkpoint)) != NONE;
}

/*
 * kv_table_item::get --
 *     Get the corresponding value. Return NONE if not found. Throw an exception on error.
 */
data_value
kv_table_item::get(kv_transaction_snapshot_ptr txn_snapshot, txn_id_t txn_id,
  timestamp_t read_timestamp, timestamp_t stable_timestamp) const
{
    std::lock_guard lock_guard(_lock);
    /*
     * Note: unlike other ops, we need to search by prepare timestamp and commit timestamps
     * together, so we can consider transactions that are only prepared and not committed. The
     * common approach is to check for prepared transactions and in that case flag a conflict, but
     * for reads this can be incorrect. See more specific rules below.
     */
    kv_update::prepare_timestamp_comparator cmp;

    if (_updates.empty())
        return NONE;

    /*
     * See if the transaction wrote something - we read our own writes, irrespective of the read
     * timestamp.
     */
    if (txn_id != k_txn_none)
        for (auto i = _updates.rbegin(); i != _updates.rend(); i++)
            if ((*i)->txn_id() == txn_id)
                return (*i)->value();

    /* Otherwise do a regular read using the transaction's read timestamp and snapshot. */
    auto i = std::upper_bound(_updates.begin(), _updates.end(), read_timestamp, cmp);

    /* Non-transaction rules: return the most recent visible value. */
    if (!txn_snapshot) {
        if (stable_timestamp != k_timestamp_latest)
            throw model_exception(
              "If the stable timestamp is set, the transaction snapshot must be set also");
        while (i != _updates.begin()) {
            const std::shared_ptr<kv_update> &u = *(--i);
            /*
             * If our read timestamp can see a prepared commit, raise a conflict (due to how our
             * std::upper_bound works, we can only see prepares before our timestamps).
             */
            if (u->prepared())
                throw wiredtiger_exception(WT_PREPARE_CONFLICT);
            else if (u->committed())
                /* All else aside, committed updates before our read timestamp are visible. */
                return u->value();
        }
        return NONE;
    } else {
        /* Transaction read rules. */
        while (i != _updates.begin()) {
            const std::shared_ptr<kv_update> &u = *(--i);
            /* Read the update if it's in the transaction's snapshot. */
            if (txn_snapshot->contains(*u) && u->durable_timestamp() <= stable_timestamp) {
                /*
                 * Only raise an error for prepared updates by transactions that weren't active when
                 * the transaction started. Updates prepared before the start of this transaction
                 * are ignored, unless they are also committed before this read, then they are
                 * visible.
                 */
                if (u->prepared())
                    throw wiredtiger_exception(WT_PREPARE_CONFLICT);
                else
                    /* Otherwise, the update must have been committed, and we read it. */
                    assert(u->committed());
                return u->value();
            }
        }
        return NONE;
    }
}

/*
 * kv_table_item::fix_timestamps --
 *     Fix the commit and durable timestamps for the corresponding update. We need to do this,
 *     because WiredTiger transaction API specifies the commit timestamp after performing the
 *     operations, not before.
 */
void
kv_table_item::fix_timestamps(
  txn_id_t txn_id, timestamp_t commit_timestamp, timestamp_t durable_timestamp)
{
    std::lock_guard lock_guard(_lock);
    kv_update::commit_timestamp_comparator cmp;

    /*
     * Remove matching elements from the collection of updates. Note that we cannot use
     * std::equal_range here, because we are deleting from the collection, which has the side effect
     * of invalidating the "second" pointer returned from equal_range.
     */
    std::list<std::shared_ptr<kv_update>> to_fix;
    auto i = std::lower_bound(_updates.begin(), _updates.end(), k_initial_commit_timestamp, cmp);
    while (i != _updates.end() && (*i)->commit_timestamp() == k_initial_commit_timestamp)
        if ((*i)->txn_id() == txn_id) {
            to_fix.push_back(*i);
            i = _updates.erase(i);
        } else
            i++;

    for (auto &u : to_fix) {
        u->set_timestamps(commit_timestamp, durable_timestamp);
        try {
            add_update_nolock(u, false, false);
        } catch (wiredtiger_exception &e) {
            std::ostringstream ss;
            ss << "Unexpected error during the commit of transaction " << txn_id << ": ";
            ss << e.what();
            throw model_exception(ss.str());
        }
    }
}

/*
 * kv_table_item::has_prepared --
 *     Check whether the item has any prepared updates for the given timestamp.
 */
bool
kv_table_item::has_prepared(timestamp_t timestamp) const
{
    std::lock_guard lock_guard(_lock);
    return has_prepared_nolock(timestamp);
}

/*
 * kv_table_item::has_prepared_nolock --
 *     Check whether the item has any prepared updates without taking a lock.
 */
bool
kv_table_item::has_prepared_nolock(timestamp_t timestamp) const
{
    kv_update::commit_timestamp_comparator cmp;

    /*
     * Check only updates with the initial commit timestamp. An update in a prepared transaction is
     * guaranteed to have that timestamp at this point.
     */
    auto r = std::equal_range(_updates.begin(), _updates.end(), k_initial_commit_timestamp, cmp);
    for (auto i = r.first; i != r.second; i++) {
        const kv_update *u = (*i).get();
        if (u->prepared() && u->txn()->prepare_timestamp() <= timestamp)
            return true;
    }

    return false;
}

/*
 * kv_table_item::rollback_to_stable --
 *     Roll back the table item to the latest stable timestamp and transaction snapshot.
 */
void
kv_table_item::rollback_to_stable(timestamp_t timestamp, kv_transaction_snapshot_ptr snapshot)
{
    std::lock_guard lock_guard(_lock);
    for (auto i = _updates.begin(); i != _updates.end();) {
        std::shared_ptr<kv_update> u = *i;
        kv_transaction_state state = u->txn_state();
        if (state != kv_transaction_state::prepared && state != kv_transaction_state::committed)
            throw model_exception("Unexpected transaction state during RTS");

        /*
         * Remove an update if one or more of the following conditions are satisfied:
         *   1. It is not in the transaction snapshot (if provided).
         *   2. It is a prepared transaction.
         *   3. Its durable timestamp is after the stable timestamp.
         */
        if ((snapshot && !snapshot->contains(*u)) || (state == kv_transaction_state::prepared) ||
          (u->durable_timestamp() > timestamp)) {
            /* Need to remove the transaction object, so that we don't leak memory. */
            (*i)->remove_txn();
            i = _updates.erase(i);
        } else
            i++;
    }
}

/*
 * kv_table_item::rollback_updates --
 *     Roll back updates of an aborted transaction.
 */
void
kv_table_item::rollback_updates(txn_id_t txn_id)
{
    std::lock_guard lock_guard(_lock);
    for (auto i = _updates.begin(); i != _updates.end();)
        if ((*i)->txn_id() == txn_id) {
            /* Need to remove the transaction object, so that we don't leak memory. */
            (*i)->remove_txn();
            i = _updates.erase(i);
        } else
            i++;
}

} /* namespace model */
