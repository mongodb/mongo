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

#ifndef DATABASE_H
#define DATABASE_H

#include <string>
#include <map>
#include <mutex>

#include "collection.h"
#include "src/component/operation_tracker.h"

namespace test_harness {
/* Key/Value type. */
typedef std::string key_value_t;

/* Representation of the collections in memory. */
class database {
public:
    static std::string build_collection_name(const uint64_t id);

public:
    /*
     * Add a new collection, this will create the underlying collection in the database.
     */
    void add_collection(uint64_t key_count = 0);

    /* Get a collection using the id of the collection. */
    collection &get_collection(uint64_t id);

    /* Get a random collection. */
    collection &get_random_collection();

    /*
     * Retrieve the current collection count, collection names are indexed from 0 so when using this
     * take care to avoid an off by one error.
     */
    uint64_t get_collection_count();

    /* FIX-ME-Test-Framework: Replace usages of this with get_collection_ids. */
    std::vector<std::string> get_collection_names();

    std::vector<uint64_t> get_collection_ids();
    void set_timestamp_manager(timestamp_manager *tsm);
    void set_operation_tracker(operation_tracker *op_tracker);
    void set_create_config(bool use_compression, bool use_reverse_collator);

private:
    std::string _collection_create_config = "";
    scoped_session _session;
    timestamp_manager *_tsm = nullptr;
    operation_tracker *_operation_tracker = nullptr;
    uint64_t _next_collection_id = 0;
    std::map<uint64_t, collection> _collections;
    std::mutex _mtx;
};
} // namespace test_harness

#endif
