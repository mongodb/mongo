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

#include "model/kv_table_item.h"
#include "wiredtiger.h"

namespace model {

/*
 * kv_table_item::add_update --
 *     Add an update.
 */
int
kv_table_item::add_update(kv_update &&update, bool must_exist, bool must_not_exist)
{
    std::lock_guard lock_guard(_lock);
    kv_update::timestamp_comparator cmp;

    /* If this is a non-timestamped update, there cannot be existing timestamped updates. */
    if (update.global()) {
        if (!_updates.empty() && !_updates.back()->global())
            return EINVAL;
    }

    /* Position the update. */
    auto i = std::upper_bound(_updates.begin(), _updates.end(), &update, cmp);

    /* If need be, fail if the key does not exist. */
    if (must_exist) {
        if (_updates.empty())
            return WT_NOTFOUND;

        auto j = i;
        if (j == _updates.begin() || (*(--j))->value() == NONE)
            return WT_NOTFOUND;
    }

    /* If need be, fail if the key exists. */
    if (must_not_exist && !_updates.empty()) {
        auto j = i;
        if (j != _updates.begin() && (*(--j))->value() != NONE)
            return WT_DUPLICATE_KEY;
    }

    /* Insert. */
    _updates.insert(i, std::shared_ptr<kv_update>(new kv_update(std::move(update))));
    return 0;
}

/*
 * kv_table_item::contains_any --
 *     Check whether the table contains the given value. If there are multiple value associated with
 *     the given timestamp, return true if any of them match.
 */
bool
kv_table_item::contains_any(const data_value &value, timestamp_t timestamp)
{
    std::lock_guard lock_guard(_lock);
    kv_update::timestamp_comparator cmp;

    /* Position the cursor on the update that is right after the provided timestamp. */
    auto i = std::upper_bound(_updates.begin(), _updates.end(), timestamp, cmp);

    /*
     * If we are positioned at the beginning of the list, there are no visible updates given the
     * provided timestamp (i.e., with timestamp that is smaller than or equal to the provided
     * timestamp).
     */
    if (i == _updates.begin())
        return false;

    /* Read the timestamp of the latest visible update. */
    timestamp_t t = (*(--i))->timestamp();

    /* Check all updates with that timestamp. */
    for (; (*i)->timestamp() == t; i--) {
        if ((*i)->value() == value)
            return true;
        if (i == _updates.begin())
            break;
    }
    return false;
}

/*
 * kv_table_item::get --
 *     Get the corresponding value. Note that this returns a copy of the object.
 */
data_value
kv_table_item::get(timestamp_t timestamp)
{
    std::lock_guard lock_guard(_lock);
    kv_update::timestamp_comparator cmp;

    if (_updates.empty())
        return NONE;

    auto i = std::upper_bound(_updates.begin(), _updates.end(), timestamp, cmp);
    if (i == _updates.begin())
        return NONE;
    return (*(--i))->value();
}

} /* namespace model */
