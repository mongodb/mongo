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

#include "model/data_value.h"
#include "model/kv_transaction.h"

namespace model {

class transaction;

/*
 * kv_update --
 *     The data value stored in a KV table, together with the relevant update information, such as
 *     the timestamp.
 */
class kv_update {

public:
    /*
     * kv_update::timestamp_comparator --
     *     The comparator that uses timestamps only.
     */
    struct timestamp_comparator {

        /*
         * kv_update::timestamp_comparator::operator() --
         *     Compare two updates.
         */
        bool
        operator()(const kv_update &left, const kv_update &right) const noexcept
        {
            return left._commit_timestamp < right._commit_timestamp;
        }

        /*
         * kv_update::timestamp_comparator::operator() --
         *     Compare the update to the given timestamp.
         */
        bool
        operator()(const kv_update &left, timestamp_t timestamp) const noexcept
        {
            return left._commit_timestamp < timestamp;
        }

        /*
         * kv_update::timestamp_comparator::operator() --
         *     Compare the update to the given timestamp.
         */
        bool
        operator()(timestamp_t timestamp, const kv_update &right) const noexcept
        {
            return timestamp < right._commit_timestamp;
        }

        /*
         * kv_update::timestamp_comparator::operator() --
         *     Compare two updates.
         */
        bool
        operator()(const kv_update *left, const std::shared_ptr<kv_update> &right) const noexcept
        {
            if (!left) /* handle nullptr */
                return !right ? false : true;
            return left->_commit_timestamp < right->_commit_timestamp;
        }

        /*
         * kv_update::timestamp_comparator::operator() --
         *     Compare the update to the given timestamp.
         */
        bool
        operator()(const std::shared_ptr<kv_update> &left, timestamp_t timestamp) const noexcept
        {
            if (!left) /* handle nullptr */
                return true;
            return left->_commit_timestamp < timestamp;
        }

        /*
         * kv_update::timestamp_comparator::operator() --
         *     Compare the update to the given timestamp.
         */
        bool
        operator()(timestamp_t timestamp, const std::shared_ptr<kv_update> &right) const noexcept
        {
            if (!right) /* handle nullptr */
                return false;
            return timestamp < right->_commit_timestamp;
        }
    };

    /*
     * kv_update::kv_update --
     *     Create a new instance.
     */
    inline kv_update(const data_value &value, timestamp_t timestamp) noexcept
        : _value(value), _commit_timestamp(timestamp), _durable_timestamp(timestamp),
          _txn_id(k_txn_none), _wt_txn_id(k_txn_none), _wt_base_write_gen(k_write_gen_none)
    {
    }

    /*
     * kv_update::kv_update --
     *     Create a new instance.
     */
    inline kv_update(const data_value &value, kv_transaction_ptr txn) noexcept
        : _value(value), _commit_timestamp(txn ? txn->commit_timestamp() : k_timestamp_none),
          _durable_timestamp(txn ? txn->durable_timestamp() : k_timestamp_none), _txn(txn),
          _txn_id(txn ? txn->id() : k_txn_none), _wt_txn_id(k_txn_none),
          _wt_base_write_gen(k_write_gen_none)
    {
    }

    /*
     * kv_update::operator== --
     *     Compare to another instance.
     */
    inline bool
    operator==(const kv_update &other) const noexcept
    {
        return _value == other._value && _txn_id == other._txn_id &&
          _commit_timestamp == other._commit_timestamp &&
          _durable_timestamp == other._durable_timestamp;
    }

    /*
     * kv_update::operator!= --
     *     Compare to another instance.
     */
    inline bool
    operator!=(const kv_update &other) const noexcept
    {
        return !(*this == other);
    }

    /*
     * kv_update::operator< --
     *     Compare to another instance.
     */
    inline bool
    operator<(const kv_update &other) const noexcept
    {
        if (_commit_timestamp != other._commit_timestamp)
            return _commit_timestamp < other._commit_timestamp;
        if (_txn_id != other._txn_id)
            return _txn_id < other._txn_id;
        if (_durable_timestamp != other._durable_timestamp)
            return _durable_timestamp < other._durable_timestamp;
        if (_value != other._value)
            return _value < other._value;
        return true;
    }

    /*
     * kv_update::operator<= --
     *     Compare to another instance.
     */
    inline bool
    operator<=(const kv_update &other) const noexcept
    {
        return *this < other || *this == other;
    }

    /*
     * kv_update::operator> --
     *     Compare to another instance.
     */
    inline bool
    operator>(const kv_update &other) const noexcept
    {
        return !(*this <= other);
    }

    /*
     * kv_update::operator>= --
     *     Compare to another instance.
     */
    inline bool
    operator>=(const kv_update &other) const noexcept
    {
        return !(*this < other);
    }

    /*
     * kv_update::value --
     *     Get the value. Note that this returns a copy of the object.
     */
    inline data_value
    value() const noexcept
    {
        return _value;
    }

    /*
     * kv_update::global --
     *     Check if this is a globally-visible, non-timestamped update.
     */
    inline bool
    global() const noexcept
    {
        return _commit_timestamp == k_timestamp_none;
    }

    /*
     * kv_update::commit_timestamp --
     *     Get the commit timestamp.
     */
    inline timestamp_t
    commit_timestamp() const noexcept
    {
        return _commit_timestamp;
    }

    /*
     * kv_update::durable_timestamp --
     *     Get the durable timestamp.
     */
    inline timestamp_t
    durable_timestamp() const noexcept
    {
        return _durable_timestamp;
    }

    /*
     * kv_update::committed --
     *     Check whether the transaction is committed.
     */
    inline bool
    committed() const noexcept
    {
        if (!_txn)
            return true;
        return _txn->state() == kv_transaction_state::committed;
    }

    /*
     * kv_update::prepared --
     *     Check whether the transaction is prepared.
     */
    inline bool
    prepared() const noexcept
    {
        if (!_txn)
            return false;
        return _txn->state() == kv_transaction_state::prepared;
    }

    /*
     * kv_update::txn --
     *     Get the transaction pointer, if available.
     */
    inline kv_transaction_ptr
    txn() const noexcept
    {
        return _txn;
    }

    /*
     * kv_update::txn_id --
     *     Get the transaction ID.
     */
    inline txn_id_t
    txn_id() const noexcept
    {
        return _txn_id;
    }

    /*
     * kv_update::txn_state --
     *     Get the update's transaction state.
     */
    inline kv_transaction_state
    txn_state() const noexcept
    {
        if (!_txn)
            return kv_transaction_state::committed;
        return _txn->state();
    }

    /*
     * kv_update::set_timestamps --
     *     Set the commit and durable timestamps. This can be called only when this object is not
     *     inserted in a sorted list.
     */
    inline void
    set_timestamps(timestamp_t timestamp, timestamp_t durable_timestamp) noexcept
    {
        _commit_timestamp = timestamp;
        _durable_timestamp = durable_timestamp;
    }

    /*
     * kv_update::remove_txn --
     *     Remove the transaction information. This can be only done if the transaction is already
     *     committed to save memory.
     */
    inline void
    remove_txn() noexcept
    {
        _txn.reset();
    }

    /*
     * kv_update::set_wt_metadata --
     *     If this update was imported from WiredTiger, remember the corresponding transaction
     *     metadata.
     */
    inline void
    set_wt_transaction_metadata(txn_id_t wt_txn_id, write_gen_t wt_base_write_gen)
    {
        _wt_txn_id = wt_txn_id;
        _wt_base_write_gen = wt_base_write_gen;
    }

    /*
     * kv_update::wt_txn_id --
     *     Get the WiredTiger transaction ID, if available.
     */
    inline txn_id_t
    wt_txn_id() const
    {
        return _wt_txn_id;
    }

    /*
     * kv_update::wt_base_write_gen --
     *     Get the WiredTiger base write generation number, if available.
     */
    inline write_gen_t
    wt_base_write_gen() const
    {
        return _wt_base_write_gen;
    }

private:
    timestamp_t _commit_timestamp;
    timestamp_t _durable_timestamp;
    data_value _value;

    /*
     * The transaction information: the transaction ID, and the pointer to the transaction object.
     * The pointer can be a nullptr if the transaction has already committed so that we don't
     * unnecessarily keep many transaction objects around. After the transaction has committed, we
     * still remember the transaction ID so that we can determine whether the update should be
     * included in a transaction snapshot.
     */
    txn_id_t _txn_id;
    kv_transaction_ptr _txn;

    /* Transaction information for updates imported from WiredTiger's debug log. */
    txn_id_t _wt_txn_id;
    write_gen_t _wt_base_write_gen;
};

} /* namespace model */
