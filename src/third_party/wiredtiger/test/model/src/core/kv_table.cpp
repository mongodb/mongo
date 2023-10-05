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

#include "model/kv_table.h"
#include "wiredtiger.h"

namespace model {

/*
 * kv_table::contains_any --
 *     Check whether the table contains the given key-value pair. If there are multiple values
 *     associated with the given timestamp, return true if any of them match.
 */
bool
kv_table::contains_any(const data_value &key, const data_value &value, timestamp_t timestamp)
{
    kv_table_item *item = item_if_exists(key);
    if (item == nullptr)
        return false;
    return item->contains_any(value, timestamp);
}

/*
 * kv_table::get --
 *     Get the value. Note that this returns a copy of the object.
 */
data_value
kv_table::get(const data_value &key, timestamp_t timestamp)
{
    kv_table_item *item = item_if_exists(key);
    if (item == nullptr)
        return NONE;
    return item->get(timestamp);
}

/*
 * kv_table::insert --
 *     Insert into the table.
 */
int
kv_table::insert(
  const data_value &key, const data_value &value, timestamp_t timestamp, bool overwrite)
{
    return item(key).add_update(std::move(kv_update(value, timestamp)), false, !overwrite);
}

/*
 * kv_table::remove --
 *     Delete a value from the table. Return true if the value was deleted.
 */
int
kv_table::remove(const data_value &key, timestamp_t timestamp)
{
    kv_table_item *item = item_if_exists(key);
    if (item == nullptr)
        return WT_NOTFOUND;
    return item->add_update(std::move(kv_update(NONE, timestamp)), true, false);
}

/*
 * kv_table::update --
 *     Update a key in the table.
 */
int
kv_table::update(
  const data_value &key, const data_value &value, timestamp_t timestamp, bool overwrite)
{
    return item(key).add_update(std::move(kv_update(value, timestamp)), !overwrite, false);
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

} /* namespace model */
