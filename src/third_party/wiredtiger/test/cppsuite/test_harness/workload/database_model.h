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

#ifndef DATABASE_MODEL_H
#define DATABASE_MODEL_H

#include <atomic>
#include <map>
#include <chrono>
#include <string>
#include <tuple>

#include "../timestamp_manager.h"
#include "random_generator.h"
#include "workload_tracking.h"

namespace test_harness {

/* Key/Value type. */
typedef std::string key_value_t;

/* A collection is made of mapped key value objects. */
class collection {
    public:
    collection(const uint64_t id, const uint64_t key_count, const std::string &name)
        : id(id), _key_count(key_count), name(name)
    {
    }

    /* Copies aren't allowed. */
    collection(const collection &) = delete;
    collection &operator=(const collection &) = delete;

    uint64_t
    get_key_count() const
    {
        return (_key_count);
    }

    /*
     * Adding new keys should generally be singly threaded per collection. If two threads both
     * attempt to add keys using the incrementing id pattern they'd frequently conflict.
     *
     * The usage pattern is:
     *   1. Call get_key_count to get the number of keys already existing. Add keys with id's equal
     *      to and greater than this value.
     *   2. Once the transaction has successfully committed then call increase_key_count() with the
     *      number of added keys.
     *
     * The set of keys should always be contiguous such that other threads calling get_key_count
     * will always know that the keys in existence are 0 -> _key_count - 1.
     */
    void
    increase_key_count(uint64_t increment)
    {
        _key_count += increment;
    }

    const std::string name;
    const uint64_t id;

    private:
    std::atomic<uint64_t> _key_count{0};
};

/* Representation of the collections in memory. */
class database {
    public:
    /*
     * Add a new collection, this will create the underlying collection in the database.
     */
    void
    add_collection(uint64_t key_count = 0)
    {
        std::lock_guard<std::mutex> lg(_mtx);
        if (_session.get() == nullptr)
            _session = connection_manager::instance().create_session();
        uint64_t next_id = _next_collection_id++;
        std::string collection_name = build_collection_name(next_id);
        /* FIX-ME-Test-Framework: This will get removed when we split the model up. */
        _collections.emplace(std::piecewise_construct, std::forward_as_tuple(next_id),
          std::forward_as_tuple(next_id, key_count, collection_name));
        testutil_check(
          _session->create(_session.get(), collection_name.c_str(), DEFAULT_FRAMEWORK_SCHEMA));
        _tracking->save_schema_operation(
          tracking_operation::CREATE_COLLECTION, next_id, _tsm->get_next_ts());
    }

    /* Get a collection using the id of the collection. */
    collection &
    get_collection(uint64_t id)
    {
        std::lock_guard<std::mutex> lg(_mtx);
        const auto it = _collections.find(id);
        if (it == _collections.end())
            testutil_die(EINVAL, "tried to get collection that doesn't exist.");
        return (it->second);
    }

    /* Get a random collection. */
    collection &
    get_random_collection()
    {
        size_t collection_count = get_collection_count();
        /* Any caller should expect at least one collection to exist. */
        testutil_assert(collection_count != 0);
        return (get_collection(
          random_generator::instance().generate_integer<uint64_t>(0, collection_count - 1)));
    }

    /*
     * Retrieve the current collection count, collection names are indexed from 0 so when using this
     * take care to avoid an off by one error.
     */
    uint64_t
    get_collection_count()
    {
        std::lock_guard<std::mutex> lg(_mtx);
        return (_collections.size());
    }

    /* FIX-ME-Test-Framework: Replace usages of this with get_collection_ids. */
    std::vector<std::string>
    get_collection_names()
    {
        std::lock_guard<std::mutex> lg(_mtx);
        std::vector<std::string> collection_names;

        for (auto const &it : _collections)
            collection_names.push_back(it.second.name);

        return (collection_names);
    }

    std::vector<uint64_t>
    get_collection_ids()
    {
        std::lock_guard<std::mutex> lg(_mtx);
        std::vector<uint64_t> collection_ids;

        for (auto const &it : _collections)
            collection_ids.push_back(it.first);

        return (collection_ids);
    }

    static std::string
    build_collection_name(const uint64_t id)
    {
        return (std::string("table:collection_" + std::to_string(id)));
    }

    void
    set_timestamp_manager(timestamp_manager *tsm)
    {
        testutil_assert(_tsm == nullptr);
        _tsm = tsm;
    }

    void
    set_workload_tracking(workload_tracking *tracking)
    {
        testutil_assert(_tracking == nullptr);
        _tracking = tracking;
    }

    private:
    scoped_session _session;
    timestamp_manager *_tsm = nullptr;
    workload_tracking *_tracking = nullptr;
    uint64_t _next_collection_id = 0;
    std::map<uint64_t, collection> _collections;
    std::mutex _mtx;
};
} // namespace test_harness

#endif
