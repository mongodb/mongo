/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "mongo/platform/mutex.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
/**
 * A Read-Copy-Update map where multiple threads can access and modify the map concurrently. All
 * Value's of the map will be shared across accessing threads and copies of the
 * ConcurrentSharedValuesMap, so thread-safe methods must be used in order to avoid issues.
 *
 * Whenever the map is modified, it will perform a copy of the current map with the modification and
 * redirect new readers to the new map. The old map version will be freed from memory once all
 * references to it are gone. This extends to the values returned by a find operation.
 */
template <typename Key, typename Value, typename... ExtraAbslArgs>
class ConcurrentSharedValuesMap {
public:
    using Map = absl::flat_hash_map<Key, std::shared_ptr<Value>, ExtraAbslArgs...>;

    ConcurrentSharedValuesMap() : _map(std::make_shared<Map>()) {}

    /**
     * Constructs a copy of the map. The map will point to the same values of the original map.
     */
    ConcurrentSharedValuesMap(const ConcurrentSharedValuesMap& other)
        : _map(atomic_load(&other._map)) {}
    /**
     * Operations other than creating a full new copy of the map can be sources of potential
     * concurrency issues. Disallow operator= and the move constructor to avoid misuse.
     */
    ConcurrentSharedValuesMap(ConcurrentSharedValuesMap&& other) = delete;
    ConcurrentSharedValuesMap& operator=(const ConcurrentSharedValuesMap& other) = delete;
    ConcurrentSharedValuesMap& operator=(ConcurrentSharedValuesMap&& other) = delete;

    /**
     * Returns a shared_ptr of the value associated with the given key.
     *
     * This is a lock-free operation and will not block under any circumstances.
     */
    std::shared_ptr<Value> find(const Key& key) const {
        auto currentMap = atomic_load(&_map);
        if (auto it = currentMap->find(key); it != currentMap->end()) {
            return it->second;
        }
        return nullptr;
    }

    /**
     * Performs an atomic find or insert operation. The returned value will contain the newly
     * created element or the previously existing one.
     *
     * Existing copies of the map or values previously obtained via find() will not be invalidated
     * by the insertion.
     *
     * This is a blocking operation as all writes are serialized.
     */
    template <typename... Args>
    std::shared_ptr<Value> getOrEmplace(const Key& key, Args&&... args) {
        stdx::lock_guard lk(_mapModificationMutex);
        auto currentMap = atomic_load(&_map);
        // Check in case it already exists.
        if (auto it = currentMap->find(key); it != currentMap->end()) {
            return it->second;
        }
        auto mapCopy = std::make_shared<Map>(*currentMap);
        auto newValue = std::make_shared<Value>(std::forward<Args>(args)...);
        auto [it, _] = mapCopy->emplace(key, std::move(newValue));
        atomic_store(&_map, std::move(mapCopy));
        return it->second;
    }

    /**
     * Erases the key from the map. Existing copies of the map or values previously obtained via
     * find() will not be invalidated.
     *
     * This is a blocking operation as all writes are serialized.
     */
    void erase(const Key& key) {
        stdx::lock_guard lk(_mapModificationMutex);
        auto currentMap = atomic_load(&_map);
        auto mapCopy = std::make_shared<Map>(*currentMap);
        mapCopy->erase(key);
        atomic_store(&_map, std::move(mapCopy));
    }

    /**
     * Clears the map, erasing all entries. Existing copies of the map or values previously obtained
     * via find() will not be invalidated.
     *
     * This is a blocking operation as all writes are serialized.
     */
    void clear() {
        stdx::lock_guard lk(_mapModificationMutex);
        auto emptyMap = std::make_shared<Map>();
        atomic_store(&_map, std::move(emptyMap));
    }

    /**
     * Updates the map atomically, this method accepts a function that will return the new map to
     * use. Existing copies of the map or values previously obtained via find() will not be
     * invalidated.
     *
     * This is a blocking operation as all writes are serialized. As the function can be expensive
     * to execute, care must be taken in order to minimize its cost. Otherwise it risks blocking
     * writes for longer than desired.
     */
    template <typename F>
    void updateWith(F&& updateFunc) {
        static_assert(std::is_invocable_r_v<Map, F, const Map&>,
                      "Function must be of type Map(const Map&)");
        stdx::lock_guard lk(_mapModificationMutex);
        auto currentMap = atomic_load(&_map);
        auto newMap = std::make_shared<Map>(std::forward<F>(updateFunc)(*currentMap));
        atomic_store(&_map, std::move(newMap));
    }

    /**
     * Returns the current map. This map will not observe changes to it performed concurrently. That
     * is to say that the map won't see changes to the key-value pairings performed by other
     * threads.
     */
    std::shared_ptr<const Map> getUnderlyingSnapshot() const {
        return atomic_load(&_map);
    }

private:
    ConcurrentSharedValuesMap(std::shared_ptr<Map> otherMap) : _map(std::move(otherMap)){};

    // shared_ptr in order to allow lock-free reads of the values.
    std::shared_ptr<Map> _map;
    // Locked in order to modify the map by either inserting or removing an element.
    Mutex _mapModificationMutex;
};
}  // namespace mongo
