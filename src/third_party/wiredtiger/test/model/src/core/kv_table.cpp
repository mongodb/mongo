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
#include <cstring>
#include <iostream>
#include <iterator>

#include "model/kv_database.h"
#include "model/kv_table.h"
#include "model/kv_transaction.h"
#include "model/util.h"
#include "wiredtiger.h"

namespace model {

/*
 * kv_table::type_by_key_value_format --
 *     Infer the table type from the key and value formats.
 */
kv_table_type
kv_table::type_by_key_value_format(const std::string &key_format, const std::string &value_format)
{
    if (key_format == "r") {
        /* Skip leading digits for the value format. */
        const char *f = value_format.c_str();
        if (isdigit(f[0]))
            (void)parse_uint64(f, &f);
        return strcmp(f, "t") == 0 ? kv_table_type::column_fix : kv_table_type::column;
    }

    return kv_table_type::row;
}

/*
 * kv_table::contains_any --
 *     Check whether the table contains the given key-value pair. If there are multiple values
 *     associated with the given timestamp, return true if any of them match.
 */
bool
kv_table::contains_any(const data_value &key, const data_value &value, timestamp_t timestamp) const
{
    const kv_table_item *item = item_if_exists(key);
    if (item == nullptr)
        return false;
    return item->contains_any(value, fix_timestamp(timestamp));
}

/*
 * kv_table::contains_any --
 *     Check whether the table contains the given key-value pair. If there are multiple values
 *     associated with the given timestamp, return true if any of them match.
 */
bool
kv_table::contains_any(kv_checkpoint_ptr ckpt, const data_value &key, const data_value &value) const
{
    const kv_table_item *item = item_if_exists(key);
    if (item == nullptr)
        return false;
    return item->contains_any(std::move(ckpt), value);
}

/*
 * kv_table::get --
 *     Get the value. Return a copy of the value if is found, or NONE if not found. Throw an
 *     exception on error.
 */
data_value
kv_table::get(const data_value &key, timestamp_t timestamp) const
{
    timestamp_t t = fix_timestamp(timestamp);
    if (t < _database.oldest_timestamp())
        throw wiredtiger_exception(EINVAL);

    const kv_table_item *item = item_if_exists(key);
    if (item == nullptr)
        return NONE;
    return fix_get(item->get(t));
}

/*
 * kv_table::get --
 *     Get the value. Return a copy of the value if is found, or NONE if not found. Throw an
 *     exception on error.
 */
data_value
kv_table::get(kv_checkpoint_ptr ckpt, const data_value &key, timestamp_t timestamp) const
{
    timestamp_t t = fix_timestamp(timestamp);
    if (t < _database.oldest_timestamp())
        throw wiredtiger_exception(EINVAL);

    const kv_table_item *item = item_if_exists(key);
    if (item == nullptr)
        return NONE;
    return fix_get(item->get(std::move(ckpt), t));
}

/*
 * kv_table::get --
 *     Get the value. Return a copy of the value if is found, or NONE if not found. Throw an
 *     exception on error.
 */
data_value
kv_table::get(kv_transaction_ptr txn, const data_value &key) const
{
    const kv_table_item *item = item_if_exists(key);
    if (item == nullptr)
        return NONE;
    return fix_get(timestamped() ? item->get(std::move(txn)) : item->get_latest(std::move(txn)));
}

/*
 * kv_table::get_ext --
 *     Get the value and return the error code instead of throwing an exception.
 */
int
kv_table::get_ext(const data_value &key, data_value &out, timestamp_t timestamp) const
{
    try {
        out = get(key, fix_timestamp(timestamp));
        return out == NONE ? WT_NOTFOUND : 0;
    } catch (wiredtiger_exception &e) {
        out = NONE;
        return e.error();
    }
}

/*
 * kv_table::get_ext --
 *     Get the value and return the error code instead of throwing an exception.
 */
int
kv_table::get_ext(
  kv_checkpoint_ptr ckpt, const data_value &key, data_value &out, timestamp_t timestamp) const
{
    try {
        out = get(std::move(ckpt), key, fix_timestamp(timestamp));
        return out == NONE ? WT_NOTFOUND : 0;
    } catch (wiredtiger_exception &e) {
        out = NONE;
        return e.error();
    }
}

/*
 * kv_table::get_ext --
 *     Get the value and return the error code instead of throwing an exception.
 */
int
kv_table::get_ext(kv_transaction_ptr txn, const data_value &key, data_value &out) const
{
    try {
        out = get(std::move(txn), key);
        return out == NONE ? WT_NOTFOUND : 0;
    } catch (wiredtiger_exception &e) {
        out = NONE;
        return e.error();
    }
}

/*
 * kv_table::insert --
 *     Insert into the table (non-transactional API).
 */
int
kv_table::insert(
  const data_value &key, const data_value &value, timestamp_t timestamp, bool overwrite)
{
    return with_transaction(
      [&](auto txn) { return insert(std::move(txn), key, value, overwrite); }, timestamp);
}

/*
 * kv_table::insert --
 *     Insert into the table.
 */
int
kv_table::insert(
  kv_transaction_ptr txn, const data_value &key, const data_value &value, bool overwrite)
{
    std::shared_ptr<kv_update> update = fix_timestamps(std::make_shared<kv_update>(value, txn));
    try {
        item(key).add_update(update, false, !overwrite);
        txn->add_update(*this, key, std::move(update));
        return 0;
    } catch (wiredtiger_exception &e) {
        return e.error();
    }
}

/*
 * kv_table::remove --
 *     Delete a value from the table (non-transactional API).
 */
int
kv_table::remove(const data_value &key, timestamp_t timestamp)
{
    return with_transaction([&](auto txn) { return remove(std::move(txn), key); }, timestamp);
}

/*
 * kv_table::remove --
 *     Delete a value from the table.
 */
int
kv_table::remove(kv_transaction_ptr txn, const data_value &key)
{
    kv_table_item *item = item_if_exists(key);
    if (item == nullptr)
        return WT_NOTFOUND;

    /*
     * If the table keys are recnos and the item only has the implicit 0 value required by recno
     * semantics, then the item we found isn't actually in the database. It should continue to be 0,
     * as that is what removals look like with recnos, and this remove should succeed while doing
     * nothing.
     */
    if (_config.type == kv_table_type::column_fix && item->implicit())
        return 0;

    std::shared_ptr<kv_update> update = fix_timestamps(
      std::make_shared<kv_update>(_config.type == kv_table_type::column_fix ? ZERO : NONE, txn));
    try {
        item->add_update(update, true, false);
        txn->add_update(*this, key, std::move(update));
        return 0;
    } catch (wiredtiger_exception &e) {
        return e.error();
    }
}

/*
 * kv_table::truncate --
 *     Truncate a key range (non-transactional API).
 */
int
kv_table::truncate(const data_value &start, const data_value &stop, timestamp_t timestamp)
{
    return with_transaction(
      [&](auto txn) { return truncate(std::move(txn), start, stop); }, timestamp);
}

/*
 * kv_table::truncate --
 *     Truncate a key range.
 */
int
kv_table::truncate(kv_transaction_ptr txn, const data_value &start, const data_value &stop)
{
    std::lock_guard lock_guard(_lock);
    if (start != model::NONE && stop != model::NONE && start > stop)
        throw model_exception("The start and the stop key are not in the right order");

    auto start_iter = start == model::NONE ? _data.begin() : _data.lower_bound(start);
    auto stop_iter = stop == model::NONE ? _data.end() : _data.upper_bound(stop);

    try {
        /* FIXME-WT-13232 Disable this check or make it FLCS only (depending on the fix). */
        /*
         * WiredTiger's implementation of truncate returns a prepare conflict if the key following
         * (or, in some cases, preceding) the truncate range belongs to a prepared transaction. In
         * the case of FLCS, skip all implicitly created items before and after the truncation
         * range.
         */
        for (auto i = stop_iter; i != _data.end(); i++) {
            if (i->second.has_prepared())
                throw known_issue_exception("WT-13232");
            if (!i->second.exists(txn) || i->second.implicit())
                continue;
            break;
        }
        for (auto i = std::reverse_iterator(start_iter); i != _data.rend(); i++) {
            if (i->second.has_prepared())
                throw known_issue_exception("WT-13232");
            if (!i->second.exists(txn) || i->second.implicit())
                continue;
            break;
        }

        for (auto i = start_iter; i != stop_iter; i++) {
            /*
             * The key does not exist for this transaction, so deletion is invalid; skip processing
             * this key.
             */
            if (!i->second.exists(txn))
                continue;

            std::shared_ptr<kv_update> update = fix_timestamps(std::make_shared<kv_update>(
              _config.type == kv_table_type::column_fix ? ZERO : NONE, txn));
            i->second.add_update(update, false, false);
            txn->add_update(*this, i->first, std::move(update));
        }
    } catch (wiredtiger_exception &e) {
        return e.error();
    }

    return 0;
}

/*
 * kv_table::update --
 *     Update a key in the table (non-transactional API).
 */
int
kv_table::update(
  const data_value &key, const data_value &value, timestamp_t timestamp, bool overwrite)
{
    return with_transaction(
      [&](auto txn) { return update(std::move(txn), key, value, overwrite); }, timestamp);
}

/*
 * kv_table::update --
 *     Update a key in the table.
 */
int
kv_table::update(
  kv_transaction_ptr txn, const data_value &key, const data_value &value, bool overwrite)
{
    std::shared_ptr<kv_update> update = fix_timestamps(std::make_shared<kv_update>(value, txn));
    try {
        item(key).add_update(update, !overwrite, false);
        txn->add_update(*this, key, std::move(update));
        return 0;
    } catch (wiredtiger_exception &e) {
        return e.error();
    }
}

/*
 * kv_table::fix_timestamps --
 *     Fix the commit and durable timestamps for the corresponding update. We need to do this,
 *     because WiredTiger transaction API specifies the commit timestamp after performing the
 *     operations, not before.
 */
void
kv_table::fix_timestamps(const data_value &key, txn_id_t txn_id, timestamp_t commit_timestamp,
  timestamp_t durable_timestamp)
{
    item(key).fix_timestamps(txn_id, commit_timestamp, durable_timestamp);
}

/*
 * kv_table::rollback_updates --
 *     Roll back updates of an aborted transaction.
 */
void
kv_table::rollback_updates(const data_value &key, txn_id_t txn_id)
{
    item(key).rollback_updates(txn_id);
}

/*
 * kv_table::clear --
 *     Clear the contents of the table.
 */
void
kv_table::clear()
{
    std::lock_guard lock_guard(_lock);
    _data.clear();
}

/*
 * kv_table::rollback_to_stable --
 *     Roll back the database table to the latest stable timestamp and transaction snapshot.
 */
void
kv_table::rollback_to_stable(timestamp_t timestamp, kv_transaction_snapshot_ptr snapshot)
{
    std::lock_guard lock_guard(_lock);

    /* RTS works only on timestamped tables. */
    if (!timestamped())
        return;

    for (auto &p : _data)
        p.second.rollback_to_stable(timestamp, snapshot);
}

/*
 * kv_table::verify_cursor --
 *     Create a verification cursor for the table. This method is not thread-safe. In fact, nothing
 *     is thread-safe until the returned cursor stops being used!
 */
kv_table_verify_cursor
kv_table::verify_cursor()
{
    return std::move(kv_table_verify_cursor(_data));
}

/*
 * kv_table::highest_recno --
 *     Get the highest recno in the table. Return 0 if the table is empty.
 */
uint64_t
kv_table::highest_recno() const
{
    std::lock_guard lock_guard(_lock);
    if (_config.type != kv_table_type::column && _config.type != kv_table_type::column_fix)
        throw model_exception("Not a column store table");
    if (_data.empty())
        return 0;
    const data_value &last = _data.rbegin()->first;
    if (!std::holds_alternative<uint64_t>(last))
        throw model_exception("The last key in the table is not a valid recno");
    return std::get<uint64_t>(last);
}

/*
 * kv_table::truncate_recnos_after --
 *     Truncate all recnos higher than the given recno on a fixed-length column store table.
 */
void
kv_table::truncate_recnos_after(uint64_t recno)
{
    std::lock_guard lock_guard(_lock);
    if (_config.type != kv_table_type::column_fix)
        throw model_exception("Not a fixed-length column store table");

    data_value r(recno);
    auto i = _data.upper_bound(r);
    if (i != _data.end())
        _data.erase(i, _data.end());
}

/*
 * kv_table::fill_missing_column_fix_recnos --
 *     Fill in missing recnos for FLCS to ensure that key ranges are contiguous.
 */
void
kv_table::fill_missing_column_fix_recnos_nolock(const data_value &key)
{
    if (_config.type != kv_table_type::column_fix)
        return;

    if (!std::holds_alternative<uint64_t>(key))
        throw model_exception("The key is not compatible with a column store: Not a recno.");
    uint64_t recno = std::get<uint64_t>(key);

    uint64_t last = 0;
    if (!_data.empty()) {
        if (!std::holds_alternative<uint64_t>(_data.begin()->first))
            throw model_exception("Invalid keys in a column store: Not a recno.");
        last = std::get<uint64_t>(_data.rbegin()->first);
    }

    for (uint64_t i = last + 1; i <= recno; i++)
        _data[data_value(i)].add_update(
          std::make_shared<kv_update>(ZERO, k_timestamp_none, true /* implicit */), false, false);
}

/*
 * kv_table::with_transaction --
 *     Run the following function within a transaction and clean up afterwards, committing the
 *     transaction if possible, and rolling it back if not.
 */
int
kv_table::with_transaction(std::function<int(kv_transaction_ptr)> fn, timestamp_t commit_timestamp)
{
    kv_transaction_ptr txn = _database.begin_transaction();
    kv_transaction_guard txn_guard(txn, commit_timestamp);

    return fn(std::move(txn));
}

} /* namespace model */
