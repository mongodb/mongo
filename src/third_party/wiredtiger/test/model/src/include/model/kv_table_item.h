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

#ifndef MODEL_KV_TABLE_ITEM_H
#define MODEL_KV_TABLE_ITEM_H

#include <deque>
#include <memory>
#include <mutex>

#include "model/data_value.h"
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
     *     Add an update.
     */
    int add_update(kv_update &&update, bool must_exist, bool overwrite);

    /*
     * kv_table_item::contains_any --
     *     Check whether the table contains the given value. If there are multiple values associated
     *     with the given timestamp, return true if any of them match.
     */
    bool contains_any(const data_value &value, timestamp_t timestamp = k_timestamp_latest);

    /*
     * kv_table_item::get --
     *     Get the corresponding value. Note that this returns a copy of the object.
     */
    data_value get(timestamp_t timestamp = k_timestamp_latest);

private:
    std::mutex _lock;
    std::deque<std::shared_ptr<kv_update>> _updates; /* sorted list of updates */
};

} /* namespace model */
#endif
