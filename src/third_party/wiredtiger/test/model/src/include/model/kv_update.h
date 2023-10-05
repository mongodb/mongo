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

#ifndef MODEL_KV_UPDATE_H
#define MODEL_KV_UPDATE_H

#include "model/data_value.h"

namespace model {

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
            return left._timestamp < right._timestamp;
        }

        /*
         * kv_update::timestamp_comparator::operator() --
         *     Compare the update to the given timestamp.
         */
        bool
        operator()(const kv_update &left, timestamp_t timestamp) const noexcept
        {
            return left._timestamp < timestamp;
        }

        /*
         * kv_update::timestamp_comparator::operator() --
         *     Compare the update to the given timestamp.
         */
        bool
        operator()(timestamp_t timestamp, const kv_update &right) const noexcept
        {
            return timestamp < right._timestamp;
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
            return left->_timestamp < right->_timestamp;
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
            return left->_timestamp < timestamp;
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
            return timestamp < right->_timestamp;
        }
    };

    /*
     * kv_update::kv_update --
     *     Create a new instance.
     */
    inline kv_update(const data_value &value, timestamp_t timestamp) noexcept
        : _value(value), _timestamp(timestamp)
    {
    }

    /*
     * kv_update::operator== --
     *     Compare to another instance.
     */
    inline bool
    operator==(const kv_update &other) const noexcept
    {
        return _value == other._value && _timestamp == other._timestamp;
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
        if (_timestamp != other._timestamp)
            return _timestamp < other._timestamp;
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
        return _timestamp == k_timestamp_none;
    }

    /*
     * kv_update::value --
     *     Get the value.
     */
    inline timestamp_t
    timestamp() const noexcept
    {
        return _timestamp;
    }

private:
    timestamp_t _timestamp;
    data_value _value;
};

} /* namespace model */
#endif
