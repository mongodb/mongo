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

#ifndef MODEL_KV_TABLE_H
#define MODEL_KV_TABLE_H

#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include "model/data_value.h"
#include "model/kv_table_item.h"
#include "model/kv_update.h"
#include "model/verify.h"
#include "wiredtiger.h"

namespace model {

/*
 * kv_table --
 *     A database table with key-value pairs.
 */
class kv_table {

public:
    /*
     * kv_table::kv_table --
     *     Create a new instance.
     */
    inline kv_table(const char *name) : _name(name) {}

    /*
     * kv_table::name --
     *     Get the name of the table. The lifetime of the returned pointer follows the lifetime of
     *     this object (given that this is a pointer to a read-only field in this class). We return
     *     this as a regular C pointer so that it can be easily used in C APIs.
     */
    inline const char *
    name() const noexcept
    {
        return _name.c_str();
    }

    /*
     * kv_table::contains_any --
     *     Check whether the table contains the given key-value pair. If there are multiple values
     *     associated with the given timestamp, return true if any of them match.
     */
    bool contains_any(
      const data_value &key, const data_value &value, timestamp_t timestamp = k_timestamp_latest);

    /*
     * kv_table::get --
     *     Get the value. Note that this returns a copy of the object.
     */
    data_value get(const data_value &key, timestamp_t timestamp = k_timestamp_latest);

    /*
     * kv_table::insert --
     *     Insert into the table.
     */
    int insert(const data_value &key, const data_value &value,
      timestamp_t timestamp = k_timestamp_none, bool overwrite = true);

    /*
     * kv_table::remove --
     *     Delete a value from the table.
     */
    int remove(const data_value &key, timestamp_t timestamp = k_timestamp_none);

    /*
     * kv_table::update --
     *     Update a key in the table.
     */
    int update(const data_value &key, const data_value &value,
      timestamp_t timestamp = k_timestamp_none, bool overwrite = true);

    /*
     * kv_table::verify --
     *     Verify the table by comparing a WiredTiger table against the model. Throw an exception on
     *     verification error.
     */
    inline void
    verify(WT_CONNECTION *connection)
    {
        kv_table_verifier(*this).verify(connection);
    }

    /*
     * kv_table::verify_noexcept --
     *     Verify the table by comparing a WiredTiger table against the model.
     */
    inline bool
    verify_noexcept(WT_CONNECTION *connection) noexcept
    {
        return kv_table_verifier(*this).verify_noexcept(connection);
    }

    /*
     * kv_table::verify_cursor --
     *     Create a verification cursor for the table. This method is not thread-safe. In fact,
     *     nothing is thread-safe until the returned cursor stops being used!
     */
    kv_table_verify_cursor verify_cursor();

protected:
    /*
     * kv_table::item --
     *     Get the item that corresponds to the given key, creating one if need be.
     */
    inline kv_table_item &
    item(const data_value &key)
    {
        std::lock_guard lock_guard(_lock);
        return _data[key]; /* this automatically instantiates the item if it does not exist */
    }

    /*
     * kv_table::item_if_exists --
     *     Get the item that corresponds to the given key, if it exists.
     */
    inline kv_table_item *
    item_if_exists(const data_value &key)
    {
        std::lock_guard lock_guard(_lock);
        auto i = _data.find(key);
        if (i == _data.end())
            return nullptr;
        return &i->second;
    }

private:
    /*
     * This data structure is designed so that the global lock is only necessary for the map
     * operations; it is okay to release the lock while the caller is still operating on the data
     * returned from the map. To keep this property going, do not remove the any elements from the
     * map. We are keeping the map sorted, so that we can easily compare the model's state with
     * WiredTiger's state. It would also help us in the future if we decide to model range scans.
     */
    std::map<data_value, kv_table_item> _data;
    std::mutex _lock;
    std::string _name;
};

} /* namespace model */
#endif
