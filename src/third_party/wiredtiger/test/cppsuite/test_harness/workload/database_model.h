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
#include <string>

namespace test_harness {

/* Key/Value type. */
typedef std::string key_value_t;

/* Representation of key states. */
struct key_t {
    bool exists;
};

/* Representation of a value. */
struct value_t {
    key_value_t value;
};

/* A collection is made of mapped Key objects. */
struct collection_t {
    std::map<key_value_t, key_t> keys;
    std::map<key_value_t, value_t> values;
};

/* Representation of the collections in memory. */
class database {
    public:
    /*
     * Add a new collection following the standard naming pattern. Currently this is the only way to
     * add collections which is supported by all components.
     */
    std::string
    add_collection()
    {
        std::lock_guard<std::mutex> lg(_mtx);
        std::string collection_name = build_collection_name(_next_collection_id);
        _collections[collection_name] = {};
        ++_next_collection_id;
        return (collection_name);
    }

    /*
     * Retrieve the current collection count, collection names are indexed from 0 so when using this
     * take care to avoid an off by one error.
     */
    uint64_t
    get_collection_count() const
    {
        return (_next_collection_id);
    }

    /*
     * Get a single collection name by id.
     */
    std::string
    get_collection_name(uint64_t id)
    {
        if (_next_collection_id <= id)
            testutil_die(id, "requested the id, %lu, of a collection that doesn't exist", id);
        return (build_collection_name(id));
    }

    std::vector<std::string>
    get_collection_names()
    {
        std::lock_guard<std::mutex> lg(_mtx);
        std::vector<std::string> collection_names;

        for (auto const &it : _collections)
            collection_names.push_back(it.first);

        return (collection_names);
    }

    std::map<key_value_t, key_t>
    get_keys(const std::string &collection_name)
    {
        std::lock_guard<std::mutex> lg(_mtx);
        return (_collections.at(collection_name).keys);
    }

    value_t
    get_record(const std::string &collection_name, const char *key)
    {
        std::lock_guard<std::mutex> lg(_mtx);
        return (_collections.at(collection_name).values.at(key));
    }

    void
    insert_record(const std::string &collection_name, const char *key, const char *value)
    {
        std::lock_guard<std::mutex> lg(_mtx);
        auto &c = _collections.at(collection_name);
        c.keys[key].exists = true;
        value_t v;
        v.value = key_value_t(value);
        c.values.emplace(key_value_t(key), v);
    }

    void
    update_record(const std::string &collection_name, const char *key, const char *value)
    {
        std::lock_guard<std::mutex> lg(_mtx);
        auto &c = _collections.at(collection_name);
        c.values.at(key).value = key_value_t(value);
    }

    void
    delete_record(const std::string &collection_name, const char *key)
    {
        std::lock_guard<std::mutex> lg(_mtx);
        auto &c = _collections.at(collection_name);
        c.keys.at(key).exists = false;
        c.values.erase(key);
    }

    private:
    /* Take a const id, not a reference as we're copying in an atomic. */
    std::string
    build_collection_name(const uint64_t id)
    {
        return (std::string("table:collection_" + std::to_string(id)));
    }
    std::atomic<uint64_t> _next_collection_id{0};
    std::map<std::string, collection_t> _collections;
    std::mutex _mtx;
};
} // namespace test_harness

#endif
